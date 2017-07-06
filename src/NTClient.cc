#include "NTClient.h"
using namespace std;

NTClient::NTClient() {
	hasError = false;
	firstConnect = true;
	connected = false;
}
bool NTClient::login(string username, string password) {
	uname = username;
	pword = password;
	bool ret = true;
	string data = string("username=");
	data += uname;
	data += "&password=";
	data += pword;
	data += "&adb=1&tz=America%2FChicago"; // No need to have anything other than Chicago timezone
	httplib::SSLClient loginReq(NITROTYPE_HOSTNAME, HTTPS_PORT);
	shared_ptr<httplib::Response> res = loginReq.post(NT_LOGIN_ENDPOINT, data, "application/x-www-form-urlencoded; charset=UTF-8");
	if (res) {
		bool foundLoginCookie = false;
		for (int i = 0; i < res->cookies.size(); ++i) {
			string cookie = res->cookies.at(i);
			addCookie(Utils::extractCKey(cookie), Utils::extractCValue(cookie));
			if (cookie.find("ntuserrem=") == 0) {
				foundLoginCookie = true;
				token = Utils::extractCValue(cookie);
				cout << "Retrieved login token: " << token << endl;
				// addCookie("ntuserrem", token);
			}
		}
		if (!foundLoginCookie) {
			ret = false;
			cout << "Unable to locate the login cookie. Maybe try a different account?\n";
		}
	} else {
		ret = false;
		cout << "Login request failed. This might be a network issue. Maybe try resetting your internet connection?\n";
	}
	bool success = getPrimusSID();
	if (!success) return false;
	return ret;
}
bool NTClient::connect() {
	wsh = new Hub();
	time_t tnow = time(0);
	rawCookieStr = Utils::stringifyCookies(&cookies);
	stringstream uristream;
	uristream  << "wss://realtime1.nitrotype.com:443/realtime/?_primuscb=" << tnow << "-0&EIO=3&transport=websocket&sid=" << primusSid << "&t=" << tnow << "&b64=1";
	string wsURI = uristream.str();
	cout << "Connecting to endpoint: " << wsURI << endl;
	if (firstConnect) {
		addListeners();
	}
	// cout << "Cookies: " << rawCookieStr << endl << endl;
	// Create override headers
	SMap customHeaders;
	customHeaders["Cookie"] = rawCookieStr;
	customHeaders["Origin"] = "https://www.nitrotype.com";
	customHeaders["User-Agent"] = "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Ubuntu Chromium/55.0.2883.87 Chrome/55.0.2883.87 Safari/537.36";
	// customHeaders["Host"] = "realtime1.nitrotype.com";
	wsh->connect(wsURI, (void*)this, customHeaders, 7000);
	// wsh->connect(wsURI);
	if (firstConnect) {
		wsh->run();
		firstConnect = false;
	}
	return true;
}
void NTClient::addCookie(string key, string val) {
	SPair sp = SPair(key, val);
	cookies.push_back(sp);
}
bool NTClient::getPrimusSID() {
	time_t tnow = time(0);
	rawCookieStr = Utils::stringifyCookies(&cookies);
	stringstream squery;
	squery << "?_primuscb=" << tnow << "-0&EIO=3&transport=polling&t=" << tnow << "-0&b64=1";
	string queryStr = squery.str();

	httplib::SSLClient loginReq(NT_REALTIME_HOST, HTTPS_PORT);
	string path = NT_PRIMUS_ENDPOINT + queryStr;
	shared_ptr<httplib::Response> res = loginReq.get(path.c_str(), rawCookieStr.c_str());
	if (res) {
		json jres = json::parse(res->body.substr(4, res->body.length()));
		primusSid = jres["sid"];
		cout << "Resolved primus SID: " << primusSid << endl;
		// addCookie("io", primusSid);
	} else {
		cout << "Error retrieving primus handshake data.\n";
		return false;
	}
	return true;
}
string NTClient::getJoinPacket(int avgSpeed) {
	stringstream ss;
	ss << "4{\"stream\":\"race\",\"msg\":\"join\",\"payload\":{\"debugging\":false,\"avgSpeed\":"
	<< avgSpeed
	<< ",\"forceEarlyPlace\":true,\"track\":\"desert\",\"music\":\"city_nights\"}}";
	return ss.str();
}
void NTClient::addListeners() {
	assert(wsh != nullptr);
	wsh->onError([this](void* udata) {
		cout << "Failed to connect to WebSocket server." << endl;
		hasError = true;
	});
	wsh->onConnection([this](WebSocket<CLIENT>* wsocket, HttpRequest req) {
		cout << "Realtime socket open" << endl;
		onConnection(wsocket, req);
	});
	wsh->onDisconnection([this](WebSocket<CLIENT>* wsocket, int code, char* msg, size_t len) {
		cout << "Disconnected from the realtime server." << endl;
		onDisconnection(wsocket, code, msg, len);
	});
	wsh->onMessage([this](WebSocket<CLIENT>* ws, char* msg, size_t len, OpCode opCode) {
		onMessage(ws, msg, len, opCode);
	});
}
void NTClient::onDisconnection(WebSocket<CLIENT>* wsocket, int code, char* msg, size_t len) {
	cout << "Disconn message: " << string(msg, len) << endl;
	cout << "Disconn code: " << code << endl;
}
void NTClient::handleData(WebSocket<CLIENT>* ws, json* j) {
	// Uncomment to dump all raw JSON packets
	// cout << "Recieved json data:" << endl << j->dump(4) << endl;
	if (j->operator[]("msg") == "setup") {
		cout << "I joined a race: " << endl << j->dump(4) << endl;
	}
}
void NTClient::onMessage(WebSocket<CLIENT>* ws, char* msg, size_t len, OpCode opCode) {
	if (opCode != OpCode::TEXT) {
		cout << "The realtime server did not send a text packet for some reason, ignoring.\n";
		return;
	}
	string smsg = string(msg, len);
	if (smsg == "3probe") {
		// Response to initial connection probe
		ws->send("5", OpCode::TEXT);
		// Join packet
		this_thread::sleep_for(chrono::seconds(1));
		string joinTo = getJoinPacket(20); // 20 WPM just to test
		ws->send(joinTo.c_str(), OpCode::TEXT);
	} else if (smsg.length() > 2 && smsg[0] == '4' && smsg[1] == '{') {
		string rawJData = smsg.substr(1, smsg.length());
		json jdata;
		try {
			jdata = json::parse(rawJData);
		} catch (const exception& e) {
			// Some error parsing real race data, something must be wrong
			cout << "There was an issue parsing server data: " << e.what() << endl;
			return;
		}
		handleData(ws, &jdata);
	} else {
		cout << "Recieved unknown WebSocket message: '" << smsg << "'\n";
	}
}
void NTClient::onConnection(WebSocket<CLIENT>* wsocket, HttpRequest req) {
	// Send a probe, which is required for connection
	wsocket->send("2probe", OpCode::TEXT);
}