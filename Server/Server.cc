#include <Server/Server.hh>

#include <thread>

#include <Server/Simulation.hh>

namespace app
{
    Server::Server()
        : m_Simulation(*this)
    {
        // websocket++ spams stdout by default (goofy)
        m_Server.set_access_channels(websocketpp::log::alevel::none);
        m_Server.init_asio();
        m_Server.set_reuse_addr(true);
        m_Server.listen(8000);
        m_Server.start_accept();
        m_Server.set_open_handler(bind(&Server::OnClientConnect, this, websocketpp::lib::placeholders::_1));
        m_Server.set_close_handler(bind(&Server::OnClientDisconnect, this, websocketpp::lib::placeholders::_1));
        m_Server.set_message_handler(bind(&Server::OnMessage, this, websocketpp::lib::placeholders::_1, websocketpp::lib::placeholders::_2));

        std::thread([&]()
                    {
            using namespace std::chrono_literals;
            while (true) {
                auto start = std::chrono::steady_clock::now();
                Tick();
                auto end = std::chrono::steady_clock::now();
                auto timeTaken = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
                std::cout << (double)timeTaken / 1'000'000.0f << "ms\n";
                auto sleepTime = std::max(0ns, 40ms - std::chrono::nanoseconds(timeTaken));
                std::this_thread::sleep_for(sleepTime);
            } })
            .detach();
    }

    void Server::Tick()
    {
        m_Simulation.Tick();
    }

    void Server::Run()
    {
        m_Server.run();
    }

    void Server::OnClientConnect(websocketpp::connection_hdl hdl)
    {
        m_Clients.push_back(new Client(hdl, m_Simulation));
    }

    void Server::OnClientDisconnect(websocketpp::connection_hdl hdl)
    {
        std::lock_guard<std::mutex> l(m_Mutex);
        for (uint64_t i = 0; i < m_Clients.size(); i++)
            if (m_Clients[i]->GetHdl().lock() == hdl.lock())
            {
                delete m_Clients[i];
                m_Clients.erase(m_Clients.begin() + i);
                return;
            }

        std::cout << "something went wrong\n";
    }

    void Server::OnMessage(websocketpp::connection_hdl hdl, WebSocketServer::message_ptr message)
    {
        for (uint64_t i = 0; i < m_Clients.size(); i++)
            if (m_Clients[i]->GetHdl().lock() == hdl.lock())
            {
                m_Clients[i]->ReadPacket((uint8_t *)message->get_raw_payload().c_str(), message->get_raw_payload().size());
            }
    }
}
