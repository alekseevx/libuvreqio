#include <thread>
#include <iostream>
#include <atomic>
#include <chrono>
#include <stdexcept>
#include <vector>
#include <memory>
#include <string>
#include <regex>
#include <random>
#include <mutex>
#include <functional>

#include <assert.h>
#include <stddef.h>
#include <stdlib.h>
#include <stdint.h>

#include <http_parser.h>
#include <uv.h>


namespace
{
	void idle(uv_idle_t *)
	{
		using ms = std::chrono::milliseconds;
		std::this_thread::sleep_for(ms(5));
	}


	class HttpRespParser final
	{
	public:
		HttpRespParser()
		{
			http_parser_settings_init(&m_settings);
			m_settings.on_message_complete = reinterpret_cast<http_cb>(&HttpRespParser::onMessage);
			this->reset();
		}

		void setOnMessageCb(std::function<void()> onMessage)
		{
			m_onMessageCb = onMessage;
		}


		void reset()
		{
			http_parser_init(&m_parser, HTTP_RESPONSE);
		}

		void execute(const void* data, size_t size)
		{
			auto d = reinterpret_cast<const char*>(data);
			http_parser_execute(&m_parser, &m_settings, d, size);
		}

		int statusCode() const
		{
			return m_parser.status_code;
		}

	private:
		static void onMessage(HttpRespParser* self)
		{
			if (self->m_onMessageCb)
				self->m_onMessageCb();
		}

	private:
		http_parser m_parser;
		http_parser_settings m_settings;
		std::function<void()> m_onMessageCb;
	};



	class TestConnect final
	{
	public:
		TestConnect(const std::string& addr, uint16_t port, uv_loop_t* loop) :
			m_addr(addr),
			m_port(port),
			m_loop(loop),
			m_sock(new uv_tcp_t)
		{
			uv_tcp_init(loop, m_sock);
			uv_tcp_nodelay(m_sock, true);

			m_connectUVReq.data = this;
			m_wrireUVReq.data = this;
			m_readUVReq.data = this;

			m_respParser.setOnMessageCb(std::bind(&TestConnect::onReqCompete, this));

			m_req = "GET /hello HTTP/1.1\r\n"
				"Host: " + addr + "\r\n"
				"Content-Type: text/plain\r\n"
				"Connection: keep-alive\r\n"
				"\r\n";

			this->startSendReq();
		}

		~TestConnect()
		{
		}

	public:
		static std::atomic<int64_t> reqCounter;

	private:
		void startSendReq(bool reconnect = true)
		{
			auto stream = reinterpret_cast<uv_handle_t*>(m_sock);

			if (reconnect)
			{
				if (uv_is_closing(stream))
					this->startConnect();
				else
					this->startClose();
			}
			else
			{
				if (uv_is_closing(stream))
					this->startConnect();
				else
					this->startWrite();
			}
		}

		void startConnect()
		{

			sockaddr_in saddr;
			uv_ip4_addr(m_addr.data(), m_port, &saddr);
			uv_tcp_connect(&m_connectUVReq, m_sock,
				reinterpret_cast<sockaddr*>(&saddr),
				&TestConnect::onConnected);
		}

		void startClose()
		{

		}

		void onReqCompete()
		{
			if (m_respParser.statusCode() == 200)
				++reqCounter;

			this->startSendReq();
		}

		void startWrite()
		{
			auto stream = reinterpret_cast<uv_stream_t*>(m_sock);
			uv_buf_t buf = {
				m_req.size(),
				const_cast<char*>(m_req.data())
			};

			uv_write(&m_wrireUVReq, stream, &buf, 1, &TestConnect::onWritten);
		}

		void startRead()
		{
			auto stream = reinterpret_cast<uv_stream_t*>(m_sock);

		}

	private:
		static void onConnected(uv_connect_t* req, int status)
		{
			TestConnect* self = reinterpret_cast<TestConnect*>(req->data);

			if (status == -1)
			{
				std::cout << "Connect failed" << std::endl;
				self->startSendReq();
				return;
			}

			self->startWrite();
		}

		static void onWritten(uv_write_t* req, int status)
		{
			TestConnect* self = reinterpret_cast<TestConnect*>(req->data);
			if (status == -1)
			{
				std::cout << "send req failed" << std::endl;
				self->startSendReq();
				return;
			}

			self->startRead();
		}

	private:
		std::string m_addr;
		uint16_t m_port;

		std::string m_req;

		uv_loop_t* m_loop = nullptr;
		uv_tcp_t* m_sock = nullptr;
		uv_connect_t m_connectUVReq;
		uv_write_t m_wrireUVReq;
		uv_read_t m_readUVReq;

		HttpRespParser m_respParser;
	};

	std::atomic<int64_t> TestConnect::reqCounter(0);
}

int main(int argc, char* argv[])
{
	auto loop = uv_loop_new();
	uv_loop_init(loop);

	uv_idle_t idler;
	uv_idle_init(loop, &idler);
	uv_idle_start(&idler, idle);

	return uv_run(loop, UV_RUN_DEFAULT);
}
