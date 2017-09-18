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
#include <list>

#include <assert.h>
#include <stddef.h>
#include <stdlib.h>
#include <stdint.h>

#include <http_parser.h>
#include <uv.h>


namespace
{
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



	class TestConnection final
	{
	public:
		TestConnection(const std::string& addr, uint16_t port, uv_loop_t* loop) :
			m_addr(addr),
			m_port(port),
			m_loop(loop),
			m_sock(nullptr)
		{
			m_connectUVReq.data = this;
			m_wrireUVReq.data = this;
			m_readUVReq.data = this;

			m_respParser.setOnMessageCb(std::bind(&TestConnection::onReqCompete, this));

			m_req = "GET /hello HTTP/1.1\r\n"
				"Host: " + addr + "\r\n"
				"Content-Type: text/plain\r\n"
				"Connection: keep-alive\r\n"
				"\r\n";
		}

		~TestConnection()
		{
		}

		void start()
		{
			this->startSendReq();
		}

	public:
		static std::atomic<int64_t> reqCounter;

	private:
		void startSendReq(bool reconnect = true)
		{
			m_respParser.reset();

			if (reconnect)
			{
				if (m_sock == nullptr)
					this->startConnect();
				else
					this->startClose();
			}
			else
				this->startWrite();
		}

		void startConnect()
		{
			m_sock = new uv_tcp_t;
			uv_tcp_init(m_loop, m_sock);
			uv_tcp_nodelay(m_sock, true);
			m_sock->data = this;

			sockaddr_in saddr;
			uv_ip4_addr(m_addr.data(), m_port, &saddr);
			uv_tcp_connect(&m_connectUVReq, m_sock,
				reinterpret_cast<sockaddr*>(&saddr),
				&TestConnection::onConnected);
		}

		void startClose()
		{
			auto stream = reinterpret_cast<uv_stream_t*>(m_sock);
			uv_read_stop(stream);

			auto handle = reinterpret_cast<uv_handle_t*>(stream);
			uv_close(handle, &TestConnection::onClosed);
		}

		void onReqCompete()
		{
			if (m_respParser.statusCode() == 200)
				++reqCounter;

			this->startSendReq(false);
		}

		void startWrite()
		{
			auto stream = reinterpret_cast<uv_stream_t*>(m_sock);
			uv_buf_t buf = {
				m_req.size(),
				const_cast<char*>(m_req.data())
			};

			uv_write(&m_wrireUVReq, stream, &buf, 1, &TestConnection::onWritten);
		}

		void startRead()
		{
			auto stream = reinterpret_cast<uv_stream_t*>(m_sock);
			stream->data = this;
			uv_read_start(stream, &TestConnection::allocBufCb, &TestConnection::onReaded);
		}

	private:
		static void onConnected(uv_connect_t* req, int status)
		{
			TestConnection* self = reinterpret_cast<TestConnection*>(req->data);

			if (status < 0)
			{
				std::cout << "Connect failed" << std::endl;
				self->startSendReq();
				return;
			}

			self->startWrite();
		}

		static void onWritten(uv_write_t* req, int status)
		{
			TestConnection* self = reinterpret_cast<TestConnection*>(req->data);
			if (status < 0)
			{
				std::cout << "send req failed" << std::endl;
				self->startSendReq();
				return;
			}

			self->startRead();
		}

		static void onReaded(uv_stream_t* stream, ssize_t nread, const uv_buf_t* buf)
		{
			TestConnection* self = reinterpret_cast<TestConnection*>(stream->data);
			if (nread <= 0)
			{
				self->startSendReq();
				return;
			}

			self->m_respParser.execute(buf->base, nread);
		}

		static void onClosed(uv_handle_t* handle)
		{
			TestConnection* self = reinterpret_cast<TestConnection*>(handle->data);
			delete self->m_sock;
			self->m_sock = nullptr;

			self->startSendReq();
		}

		static void allocBufCb(uv_handle_t* handle, size_t suggestedSize, uv_buf_t* buf)
		{
			TestConnection* self = reinterpret_cast<TestConnection*>(handle->data);
			if (self->m_readBufSize < suggestedSize)
			{
				self->m_readBuf = realloc(self->m_readBuf, suggestedSize);
				self->m_readBufSize = (self->m_readBuf != nullptr) ? suggestedSize : 0;
			}

			buf->base = reinterpret_cast<char*>(self->m_readBuf);
			buf->len = self->m_readBufSize;
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

		void* m_readBuf = nullptr;
		size_t m_readBufSize = 0;

	};
	std::atomic<int64_t> TestConnection::reqCounter(0);
	using TestConnectionList = std::list<TestConnection>;


	void statThread()
	{
		auto start = std::chrono::steady_clock::now();
		int64_t startReqCount = TestConnection::reqCounter;

		while (true)
		{
			using ms = std::chrono::milliseconds;

			std::this_thread::sleep_for(ms(2500));
			int64_t stopReqCount = TestConnection::reqCounter;
			auto stop = std::chrono::steady_clock::now();

			double duration_in_sec = std::chrono::duration_cast<ms>(stop - start).count() / 1000.0;
			double rps = static_cast<double>(stopReqCount - startReqCount) / duration_in_sec;
			std::cout << "RPS=" << rps << std::endl;

			start = stop;
			startReqCount = stopReqCount;
		}
	}


	struct NetAddr final
	{
		std::string host;
		uint16_t port;

		static NetAddr from(const std::string& s)
		{
			static const std::regex NetAddRx("(.+):(\\d+)");

			std::smatch match_res;
			if (!std::regex_match(s, match_res, NetAddRx))
				throw std::runtime_error("Invalid address format");

			std::string host = match_res[1];
			uint16_t port = std::stoi(match_res[2]);
			return { host, port };
		}
	};
	using NetAddrs = std::vector<NetAddr>;


	TestConnectionList makeConnections(const NetAddrs& netAddrs, size_t connCount, uv_loop_t* loop)
	{
		assert(!netAddrs.empty());

		std::random_device rd;
		std::mt19937 gen;
		std::uniform_int_distribution<size_t> dis(0, netAddrs.size() - 1);

		TestConnectionList ret;
		for (size_t i = 0; i < connCount; ++i)
		{
			auto& netAddr = netAddrs.size() > 1 ? netAddrs[dis(gen)] : netAddrs.front();
			ret.emplace_back(netAddr.host, netAddr.port, loop);
		}

		return ret;
	}


	struct ProgramOpts
	{
		unsigned connCount = 0;
		NetAddrs addrs;
	};

	void usage(const char* program)
	{
		std::cout << program
			<< " <connCount>"
			<< " <host:port> <host:port> ..."
			<< std::endl;
		exit(EXIT_FAILURE);
	}

	ProgramOpts parseArgs(int argc, char* argv[])
	{
		if (argc < 3)
			usage(argv[0]);

		ProgramOpts opts;
		opts.connCount = std::stoi(argv[1]);

		for (auto a = argv + 2; a != argv + argc; ++a)
			opts.addrs.push_back(NetAddr::from(*a));
		return opts;
	}
}

int main(int argc, char* argv[])
{
	auto opts = parseArgs(argc, argv);

	std::thread(&statThread).detach();

	auto loop = uv_default_loop();

	auto conns = makeConnections(opts.addrs, opts.connCount, loop);
	for (auto& conn : conns)
		conn.start();

	return uv_run(loop, UV_RUN_DEFAULT);
}
