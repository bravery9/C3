#include "StdAfx.h"
#include "DuplexConnection.h"
#include "SocketsException.h"

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
FSecure::DuplexConnection::DuplexConnection(ClientSocket sock) : m_IsSending{ false }, m_IsReceiving{ false }, m_ClientSocket{ std::move(sock) }
{
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
FSecure::DuplexConnection::DuplexConnection(std::string_view addr, uint16_t port) : DuplexConnection{ ClientSocket(addr, port) }
{
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
FSecure::DuplexConnection::~DuplexConnection()
{
	{
		std::unique_lock lock(m_MessagesMutex);
		Stop();
	}
	m_NewMessage.notify_all();
	if (m_SendingThread.joinable())
		m_SendingThread.join();
	if (m_ReceivingThread.joinable())
		m_ReceivingThread.join();
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
void FSecure::DuplexConnection::StartSending()
{
	m_IsSending = true;
	m_SendingThread = std::thread([this]
	{
		try
		{
			while (true)
			{
				ByteVector message;
				{
					std::unique_lock lock(m_MessagesMutex);
					if (!m_IsSending)
						break;
					message = GetMessage(lock);
					if (message.empty())
						break; // connection closed
				}
				m_ClientSocket.Send(message);
			}
		}
		catch (FSecure::SocketsException&)
		{
			Stop();
		}
	});
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
FSecure::ByteVector FSecure::DuplexConnection::Receive()
{
	return m_ClientSocket.Receive();
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
void FSecure::DuplexConnection::StartReceiving(std::function<void(ByteVector)> callback)
{
	m_IsReceiving = true;
	m_ReceivingThread = std::thread([this, callback]()
	{
		try
		{
			while (true)
			{
				if (m_IsReceiving && !m_ClientSocket.HasReceivedData())
				{
					std::this_thread::sleep_for(10ms);
					continue;
				}
				if (!m_IsReceiving)
					break;

				auto message = Receive();
				if (!m_IsReceiving || message.empty())
					break;
				callback(std::move(message));
			}
		}
		catch (FSecure::SocketsException&)
		{
			Stop();
		}
	});
}

void FSecure::DuplexConnection::Stop()
{
	m_IsSending = false;
	m_IsReceiving = false;
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
bool FSecure::DuplexConnection::IsSending() const
{
	return m_IsSending;
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
void FSecure::DuplexConnection::Send(ByteVector message)
{
	std::scoped_lock lock(m_MessagesMutex);
	m_Messages.emplace(std::move(message));
	m_NewMessage.notify_one();
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
FSecure::ByteVector FSecure::DuplexConnection::GetMessage(std::unique_lock<std::mutex>& lock)
{
	auto popMessage = [this]
	{
		auto ret = m_Messages.front();
		m_Messages.pop();
		return ret;
	};

	if (!m_Messages.empty())
	{
		return popMessage();
	}
	else
	{
		while (true)
		{
			m_NewMessage.wait(lock);
			if (!m_IsSending)
				return {}; // stop requested
			else if (!m_Messages.empty())
				return popMessage();
			else
				continue; // condition_variable spuriously unlocked, go back to waiting
		}
	}
}
