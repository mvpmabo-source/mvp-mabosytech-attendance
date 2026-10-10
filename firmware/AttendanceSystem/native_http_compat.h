#pragma once
#include <Arduino.h>
#include <esp_http_server.h>
#include <functional>

#ifndef RESPONSE_TRY_AGAIN
#define RESPONSE_TRY_AGAIN 0xFFFFFFFFu
#endif

typedef httpd_method_t WebRequestMethodComposite;

class AsyncWebParameter {
  String _value;
public:
  explicit AsyncWebParameter(const String& value=String()) : _value(value) {}
  const String& value() const { return _value; }
};

class AsyncWebServerResponse {
public:
  int _code;
  String _contentType;
  struct Header { String name; String value; };
  Header _headers[12];
  uint8_t _headerCount=0;
  std::function<size_t(uint8_t*,size_t,size_t)> _callback;
  std::function<void()> _disconnect[4];
  uint8_t _disconnectCount=0;
  String _body;
  const uint8_t* _data=nullptr;
  size_t _dataLen=0;
  bool _chunked=false;
public:
  AsyncWebServerResponse(int code,const char* contentType);
  virtual ~AsyncWebServerResponse() = default;
  void addHeader(const char* name,const String& value);
  void addHeader(const String& name,const String& value);
  bool isChunked() const { return _chunked; }
  friend class AsyncWebServerRequest;
  friend class AsyncProgmemResponse;
  friend class AsyncWebServer;
};

class AsyncProgmemResponse : public AsyncWebServerResponse {
public:
  AsyncProgmemResponse(int code,const char* contentType,const uint8_t* data,size_t length);
};

class AsyncWebServerRequest {
  httpd_req_t* _req;
  struct ParamKV { String name; String value; bool post; };
  ParamKV _params[32];
  uint8_t _paramCount=0;
  AsyncWebParameter _tempParam;
  std::function<void()> _disconnect[4];
  uint8_t _disconnectCount=0;
  bool _sendFailed=false;   // set when a synchronous send failed; dispatch() then closes the socket

  void parseParams(const char* encoded,bool post);
  static String urlDecode(const String& in);
  String rawHeader(const char* name) const;
  friend class AsyncWebServer;
public:
  explicit AsyncWebServerRequest(httpd_req_t* req);
  ~AsyncWebServerRequest() = default;

  bool hasHeader(const char* name) const;
  String header(const char* name) const;

  bool hasParam(const char* name,bool post=false,bool file=false) const;
  const AsyncWebParameter* getParam(const char* name,bool post=false,bool file=false);

  void onDisconnect(std::function<void()> cb);

  AsyncWebServerResponse* beginResponse(int code,const char* contentType,const char* content);
  AsyncWebServerResponse* beginResponse(int code,const char* contentType,const String& content);
  AsyncWebServerResponse* beginChunkedResponse(const char* contentType,std::function<size_t(uint8_t*,size_t,size_t)> cb);

  void send(int code,const char* contentType,const char* content);
  void send(int code,const char* contentType,const String& content);
  void send(AsyncWebServerResponse* response);

  httpd_req_t* nativeRequest() const { return _req; }
  // Record the outcome of a direct httpd_resp_send(). savedErrno must be errno read
  // immediately after the failing call. Logs failures always, successes only if logOk.
  void reportSend(const char* label,esp_err_t err,int savedErrno,uint32_t startMs,size_t bytes,bool logOk=false);
  bool sendFailed() const { return _sendFailed; }
public:
  void fireDisconnects();
};

class AsyncWebServer {
  struct Route {
    String uri;
    WebRequestMethodComposite method;
    std::function<void(AsyncWebServerRequest*)> handler;
    httpd_uri_t native{};
  };
  Route _routes[48];
  size_t _routeCount=0;
  httpd_handle_t _server=nullptr;

  static esp_err_t dispatch(httpd_req_t* req);
  esp_err_t registerRoute(Route& route);
public:
  explicit AsyncWebServer(uint16_t port=80);
  bool on(const char* uri,WebRequestMethodComposite method,std::function<void(AsyncWebServerRequest*)> handler);
  bool begin();
  void end();
  httpd_handle_t nativeServer() const { return _server; }
};