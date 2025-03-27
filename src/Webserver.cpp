#include "../inc/Webserver.hpp"

Webserver::Webserver() : _maxFd(0) {}

Webserver::~Webserver() {}

void Webserver::setupServers(std::vector<Server> servers)
{
	std::cout << "Initializing servers...\n";
	_serv = servers;
	for (std::vector<Server>::iterator it = _serv.begin(); it != _serv.end(); ++it)
	{
		bool isSameServ = false;
		for (std::vector<Server>::iterator existingServerIt = _serv.begin(); existingServerIt != it; ++existingServerIt)
		{
			if (existingServerIt->getHost() == it->getHost() && existingServerIt->getPort() == it->getPort())
			{
				it->setFd(existingServerIt->getFd());
				isSameServ = true;
			}
		}
		if (!isSameServ)
			it->setupServer();
		PrintApp::printStartServer(*it);
	}
}

void Webserver::processServerRequests()
{
	int pollStatus;
	initializePollFds();
	while (true)
	{
		pollStatus = poll(&_pollFds[0], _pollFds.size(), 1000);
		if (pollStatus < 0)
			continue;
		if (pollStatus == 0)
		{
			handleClientTimeout();
			for (std::map<int, Client>::iterator it = _clientsDict.begin(); it != _clientsDict.end(); ++it)
			{
				Client& client = it->second;
				if (client.response.getCgiFlag() == 1 && client.getCgiStartTime() > 0)
				{
					time_t now = time(NULL);
					time_t start = client.getCgiStartTime();
					const int CGI_TIMEOUT_SECONDS = 5;
					if ((now - start) > CGI_TIMEOUT_SECONDS)
					{
						kill(client.response.cgi_handler_.get_process_id(), SIGKILL);
						int status;
						waitpid(client.response.cgi_handler_.get_process_id(), &status, 0);
						removeFromPoll(client.response.cgi_handler_.pipe_in[1]);
						close(client.response.cgi_handler_.pipe_in[1]);
						removeFromPoll(client.response.cgi_handler_.pipe_out[0]);
						close(client.response.cgi_handler_.pipe_out[0]);
						client.response.toggleCgi(2);
						client.response.setError(504);
						client.setCgiStartTime(0);
						addToPoll(it->first, POLLOUT);
					}
				}
			}
			continue;
		}
		std::vector<struct pollfd> currentFds = _pollFds;
		std::set<int> handledClients;
		for (size_t i = 0; i < currentFds.size(); ++i)
		{
			int fd = currentFds[i].fd;
			short revents = currentFds[i].revents;
			if (handledClients.count(fd))
				continue;
			if (revents & POLLIN)
			{
				if (_servsDict.count(fd))
					acceptNewConnection(_servsDict.find(fd)->second);
				else
				{
					for (std::map<int, Client>::iterator it = _clientsDict.begin(); it != _clientsDict.end(); ++it)
					{
						Client& client = it->second;
						if (fd == it->first)
						{
							readAndProcessRequest(fd, client);
							handledClients.insert(fd);
							break;
						}
						else if (client.response.getCgiFlag() == 1 && fd == client.response.cgi_handler_.pipe_out[0])
						{
							readCgiResponse(client, client.response.cgi_handler_);
							handledClients.insert(fd);
							break;
						}
					}
				}
			}
			if ((revents & POLLOUT) && !handledClients.count(fd))
			{
				for (std::map<int, Client>::iterator it = _clientsDict.begin(); it != _clientsDict.end(); ++it)
				{
					Client& client = it->second;
					int cgi_state = client.response.getCgiFlag();
					if (fd == it->first && (cgi_state == 0 || cgi_state == 2))
					{
						sendResponse(fd, client);
						handledClients.insert(fd);
						break;
					}
					else if (cgi_state == 1 && fd == client.response.cgi_handler_.pipe_in[1])
					{
						sendCgiBody(client, client.response.cgi_handler_);
						handledClients.insert(fd);
						break;
					}
				}
			}
			if (revents & (POLLHUP | POLLERR | POLLNVAL))
			{
				for (std::map<int, Client>::iterator it = _clientsDict.begin(); it != _clientsDict.end(); ++it)
				{
					Client& client = it->second;
					if (client.response.getCgiFlag() == 1)
					{
						if (fd == client.response.cgi_handler_.pipe_in[1])
						{
							removeFromPoll(fd);
							close(fd);
							handledClients.insert(fd);
							break;
						}
						else if (fd == client.response.cgi_handler_.pipe_out[0])
						{
							readCgiResponse(client, client.response.cgi_handler_);
							handledClients.insert(fd);
							break;
						}
					}
					else if (fd == it->first)
					{
						closeConnection(fd);
						handledClients.insert(fd);
						break;
					}
				}
			}
		}
		handleClientTimeout();
	}
}

void Webserver::initializePollFds()
{
    _pollFds.clear();
    for (std::vector<Server>::iterator it = _serv.begin(); it != _serv.end(); ++it)
    {
    	if (listen(it->getFd(), 555) == -1)
    		PrintApp::printEvent(RED_BOLD, FAILURE, "Failed to listen on socket. Reason: %s. Closing the connection...", strerror(errno));
    	if (fcntl(it->getFd(), F_SETFL, O_NONBLOCK) < 0)
    		PrintApp::printEvent(RED_BOLD, FAILURE, "Error setting non-blocking mode using fcntl. Reason: %s. Closing the connection...", strerror(errno));
    	addToPoll(it->getFd(), POLLIN);
    	_servsDict.insert(std::make_pair(it->getFd(), *it));
    }
    _maxFd = _serv.back().getFd();
}

void Webserver::addToPoll(int fd, short events)
{
	struct pollfd pfd;
	pfd.fd = fd;
	pfd.events = events;
	pfd.revents = 0;
	_pollFds.push_back(pfd);
	if (fd > _maxFd)
	    _maxFd = fd;
}

void Webserver::removeFromPoll(int fd)
{
	for (std::vector<struct pollfd>::iterator it = _pollFds.begin(); it != _pollFds.end(); ++it)
	{
		if (it->fd == fd)
		{
			_pollFds.erase(it);
			break;
		}
	}
	if (fd == _maxFd)
	{
		_maxFd = 0;
		for (size_t i = 0; i < _pollFds.size(); ++i)
			if (_pollFds[i].fd > _maxFd)
				_maxFd = _pollFds[i].fd;
	}
}

void Webserver::acceptNewConnection(Server &serv)
{
	struct sockaddr_in cliSocketAddr;
	long cliSocketAddrSize = sizeof(cliSocketAddr);
	int cliSocket;
	Client newClient(serv);
	if ((cliSocket = accept(serv.getFd(), (struct sockaddr *)&cliSocketAddr, (socklen_t *)&cliSocketAddrSize)) == -1)
	{
		PrintApp::printEvent(RED_BOLD, SUCCESS, "Encountered an error while accepting incoming connection. Reason: %s.", strerror(errno));
		return;
	}
	PrintApp::printEvent(GREEN_BOLD, SUCCESS, "New connection established.");
	addToPoll(cliSocket, POLLIN);
	if (fcntl(cliSocket, F_SETFL, O_NONBLOCK) < 0)
	{
		PrintApp::printEvent(RED_BOLD, FAILURE, "Error encountered while setting socket to non-blocking mode using fcntl. Reason: %s. Closing the connection...", strerror(errno));
		removeFromPoll(cliSocket);
		close(cliSocket);
		return;
	}
	newClient.setSocket(cliSocket);
	if (_clientsDict.count(cliSocket) != 0)
		_clientsDict.erase(cliSocket);
	_clientsDict.insert(std::make_pair(cliSocket, newClient));
}

void Webserver::readAndProcessRequest(const int &i, Client &client)
{
	char buffer[40000];
	int bytesRead = read(i, buffer, 40000);
	if (bytesRead == 0)
	{
		PrintApp::printEvent(YELLOW, SUCCESS, "Connection with client closed.");
		closeConnection(i);
		return;
	}
	if (bytesRead < 0)
	{
		PrintApp::printEvent(RED_BOLD, SUCCESS, "Error reading from FD %d: %s.", i, strerror(errno));
		closeConnection(i);
		return;
	}
	if (bytesRead)
	{
		client.updateLastMessageTime();
		client.request.parseHTTPRequestData(buffer, bytesRead);
		memset(buffer, 0, sizeof(buffer));
	}
	if (client.request.isParsingDone() || client.request.errorCodes())
	{
		assignServer(client);
		if (client.request.errorCodes() != 501)
			PrintApp::printEvent(BLUE, SUCCESS, "<%s> \"%s\"", client.request.getMethodStr().c_str(), client.request.getPath().c_str());
		client.buildResponse();
		if (client.response.getCgiFlag())
		{
			handleReqBody(client);
			client.setCgiStartTime(time(NULL));
			addToPoll(client.response.cgi_handler_.pipe_in[1], POLLOUT);
			addToPoll(client.response.cgi_handler_.pipe_out[0], POLLIN);
			removeFromPoll(i);
		}
		else
		{
			removeFromPoll(i);
			addToPoll(i, POLLOUT);
		}
	}
}

void Webserver::assignServer(Client &client)
{
	for (std::vector<Server>::iterator it = _serv.begin(); it != _serv.end(); ++it)
	{
		if (checkServ(client, it))
			client.setServer(*it);
	}
}

bool Webserver::checkServ(Client &client, std::vector<Server>::iterator it)
{
	bool s = client.request.getServerName() == it->getServerName();
	bool h = client.server.getHost() == it->getHost();
	bool p = client.server.getPort() == it->getPort();
	return s && h && p;
}

void Webserver::handleReqBody(Client &client)
{
	if (client.request.getBody().length() == 0 && client.request.getHttpMethod() == POST)
	{
		std::string tmp;
		std::fstream file(client.response.cgi_handler_.get_script_path().c_str(), std::ios::in);
		if (file.is_open())
		{
			std::stringstream ss;
			ss << file.rdbuf();
			tmp = ss.str();
			file.close();
			client.request.setBody(tmp);
		}
		else
		{
			PrintApp::printEvent(RED_BOLD, SUCCESS, "Failed to open CGI script file for default body.");
			client.response.setError(500);
			client.response.toggleCgi(2);
		}
	}
}

void Webserver::sendCgiBody(Client &client, Cgi &cgi)
{
	int bytesSent;
	std::string &req_body = client.request.getBody();
	if (req_body.empty())
		bytesSent = 0;
	else if (req_body.length() >= 40000)
		bytesSent = write(cgi.pipe_in[1], req_body.c_str(), 40000);
	else
		bytesSent = write(cgi.pipe_in[1], req_body.c_str(), req_body.length());
	if (bytesSent < 0)
	{
		PrintApp::printEvent(RED_BOLD, SUCCESS, "Error occurred while sending CGI body. Reason: %s.", strerror(errno));
		removeFromPoll(cgi.pipe_in[1]);
		close(cgi.pipe_in[1]);
		client.response.setError(500);
		client.response.toggleCgi(2);
		addToPoll(client.getSocket(), POLLOUT);
	}
	else if (bytesSent == 0 || static_cast<size_t>(bytesSent) == req_body.length())
	{
		removeFromPoll(cgi.pipe_in[1]);
		close(cgi.pipe_in[1]);
	}
	else
	{
		client.updateLastMessageTime();
		req_body = req_body.substr(bytesSent);
	}
}

void Webserver::readCgiResponse(Client &client, Cgi &cgi)
{
	char buffer[40000 * 2];
	int bytesRead = read(cgi.pipe_out[0], buffer, 40000 * 2);
	if (bytesRead < 0)
	{
		removeFromPoll(cgi.pipe_out[0]);
		close(cgi.pipe_out[0]);
		client.response.toggleCgi(2);
		client.response.setError(500);
		addToPoll(client.getSocket(), POLLOUT);
	}
	else if (bytesRead > 0)
	{
		client.updateLastMessageTime();
		_cgiOutBuff.append(buffer, bytesRead);
		memset(buffer, 0, sizeof(buffer));
	}
	else
	{
		int status;
		pid_t result = waitpid(cgi.get_process_id(), &status, WNOHANG);
		if (result == cgi.get_process_id())
		{
			removeFromPoll(cgi.pipe_out[0]);
			close(cgi.pipe_out[0]);
			if (WEXITSTATUS(status) != 0)
				client.response.setError(502);
			else
			{
				client.response.toggleCgi(2);
				if (_cgiOutBuff.find("HTTP/") == 0)
				{
					client.response.full_response_ = _cgiOutBuff;
					std::string firstLine = _cgiOutBuff.substr(0, _cgiOutBuff.find("\r\n"));
					std::string statusStr = firstLine.substr(firstLine.find(" ") + 1, 3);
					client.response.setStatus(atoi(statusStr.c_str()));
				}
				else
				{
					client.response.setStatus(200);
					client.response.full_response_ = "HTTP/1.1 200 OK\r\n";
					client.response.full_response_.append("Content-Type: text/html\r\n");
					client.response.full_response_.append("Content-Length: " + toString(_cgiOutBuff.length()) + "\r\n");
					client.response.full_response_.append("\r\n");
					client.response.full_response_.append(_cgiOutBuff);
				}
				_cgiOutBuff = "";
			}
			addToPoll(client.getSocket(), POLLOUT);
		}
	}
}

void Webserver::sendResponse(const int &i, Client &c)
{
	int bytesSent;
	std::string response = c.response.getResponse();
	if (response.length() >= 40000)
		bytesSent = write(i, response.c_str(), 40000);
	else
		bytesSent = write(i, response.c_str(), response.length());
	if (bytesSent < 0)
	{
		PrintApp::printEvent(RED_BOLD, SUCCESS, "Error sending response to FD %d: %s.", i, strerror(errno));
		closeConnection(i);
	}
	else if (bytesSent == 0 || static_cast<size_t>(bytesSent) == response.length())
	{
		PrintApp::printEvent(WHITE, SUCCESS, "Status<%d>", c.response.getStatus());
		if (c.request.isConnectionKeepAlive() == false || c.request.errorCodes() || c.response.getCgiFlag())
		{
			PrintApp::printEvent(YELLOW, SUCCESS, "Connection with client has been terminated.");
			closeConnection(i);
		}
		else
		{
			removeFromPoll(i);
			addToPoll(i, POLLIN);
			c.clearClient();
		}
	}
	else
	{
		c.updateLastMessageTime();
		c.response.trimResponse(bytesSent);
	}
}

void Webserver::handleClientTimeout()
{
	for (std::map<int, Client>::iterator it = _clientsDict.begin(); it != _clientsDict.end();)
	{
		if (time(NULL) - it->second.getLastMessageTime() > 30)
		{
			PrintApp::printEvent(YELLOW, SUCCESS, "The connection with Client has timed out. Closing the connection...");
			int fd = it->first;
			++it;
			closeConnection(fd);
		}
		else
			++it;
	}
}

void Webserver::closeConnection(const int fd)
{
	removeFromPoll(fd);
	close(fd);
	_clientsDict.erase(fd);
}
