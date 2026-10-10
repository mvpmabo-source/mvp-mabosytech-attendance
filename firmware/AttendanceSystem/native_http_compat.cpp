#include "native_http_compat.h"
#include <cstring>
#include <cstdlib>
#include <errno.h>
#include <lwip/sockets.h>
#include <esp_heap_caps.h>

static const char* statusText(int code){
  switch(code){
    case 200:return "200 OK"; case 201:return "201 Created"; case 202:return "202 Accepted";
    case 204:return "204 No Content"; case 302:return "302 Found"; case 400:return "400 Bad Request";
    case 401:return "401 Unauthorized"; case 403:return "403 Forbidden"; case 404:return "404 Not Found";
    case 409:return "409 Conflict"; case 429:return "429 Too Many Requests"; case 500:return "500 Internal Server Error";
    case 503:return "503 Service Unavailable"; default: return "500 Internal Server Error";
  }
}

AsyncWebServerResponse::AsyncWebServerResponse(int code,const char* contentType)
  : _code(code), _contentType(contentType ? contentType : "") {}

void AsyncWebServerResponse::addHeader(const char* name,const String& value){
  if(!name || !name[0] || _headerCount >= 12) return;
  _headers[_headerCount].name=name;
  _headers[_headerCount].value=value;
  ++_headerCount;
}

void AsyncWebServerResponse::addHeader(const String& name,const String& value){
  addHeader(name.c_str(),value);
}

AsyncProgmemResponse::AsyncProgmemResponse(int code,const char* contentType,const uint8_t* data,size_t length)
  : AsyncWebServerResponse(code,contentType){
  _data=data; _dataLen=length;
}

String AsyncWebServerRequest::urlDecode(const String& in){
  String out; out.reserve(in.length());
  for(size_t i=0;i<in.length();++i){
    char c=in[i];
    if(c=='+'){ out+=' '; continue; }
    if(c=='%' && i+2<in.length()){
      char h1=in[i+1], h2=in[i+2];
      auto hex=[](char x)->int{
        if(x>='0'&&x<='9') return x-'0';
        if(x>='a'&&x<='f') return x-'a'+10;
        if(x>='A'&&x<='F') return x-'A'+10;
        return -1;
      };
      int a=hex(h1),b=hex(h2);
      if(a>=0&&b>=0){ out+=(char)((a<<4)|b); i+=2; continue; }
    }
    out+=c;
  }
  return out;
}

void AsyncWebServerRequest::parseParams(const char* encoded,bool post){
  if(!encoded || !encoded[0]) return;
  String all=encoded;
  size_t start=0;
  while(start<all.length() && _paramCount<32){
    size_t amp=all.indexOf('&',start);
    if(amp==(size_t)-1) amp=all.length();
    String item=all.substring(start,amp);
    size_t eq=item.indexOf('=');
    String key=(eq==(size_t)-1)?item:item.substring(0,eq);
    String val=(eq==(size_t)-1)?String():item.substring(eq+1);
    _params[_paramCount++] = {urlDecode(key),urlDecode(val),post};
    start=amp+1;
  }
}

String AsyncWebServerRequest::rawHeader(const char* name) const{
  if(!_req || !name) return String();
  size_t len=httpd_req_get_hdr_value_len(_req,name);
  if(len==0) return String();
  char* buf=(char*)malloc(len+1);
  if(!buf) return String();
  if(httpd_req_get_hdr_value_str(_req,name,buf,len+1)!=ESP_OK){ free(buf); return String(); }
  String out(buf);
  free(buf);
  return out;
}

AsyncWebServerRequest::AsyncWebServerRequest(httpd_req_t* req):_req(req){
  if(!_req) return;
  char query[1024];
  if(httpd_req_get_url_query_str(_req,query,sizeof(query))==ESP_OK) parseParams(query,false);

  if(_req->method==HTTP_POST || _req->method==HTTP_PUT || _req->method==HTTP_PATCH || _req->method==HTTP_DELETE){
    const size_t total=(size_t)_req->content_len;
    if(total>8192) return;
    if(total>0){
      char* buf=(char*)malloc(total+1);
      if(buf){
        size_t received=0;
        while(received<total){
          int n=httpd_req_recv(_req,buf+received,total-received);
          if(n<=0){ received=0; break; }
          received+=(size_t)n;
        }
        if(received==total){ buf[total]='\0'; parseParams(buf,true); }
        free(buf);
      }
    }
  }
}

bool AsyncWebServerRequest::hasHeader(const char* name) const{
  return rawHeader(name).length()>0;
}

String AsyncWebServerRequest::header(const char* name) const{
  return rawHeader(name);
}

bool AsyncWebServerRequest::hasParam(const char* name,bool post,bool file) const{
  (void)file;
  if(!name) return false;
  for(uint8_t i=0;i<_paramCount;++i)
    if(_params[i].post==post && _params[i].name==name) return true;
  return false;
}

const AsyncWebParameter* AsyncWebServerRequest::getParam(const char* name,bool post,bool file){
  (void)file;
  if(!name) return nullptr;
  for(uint8_t i=0;i<_paramCount;++i){
    if(_params[i].post==post && _params[i].name==name){
      _tempParam=AsyncWebParameter(_params[i].value);
      return &_tempParam;
    }
  }
  return nullptr;
}

void AsyncWebServerRequest::onDisconnect(std::function<void()> cb){
  if(cb && _disconnectCount<4) _disconnect[_disconnectCount++]=cb;
}

AsyncWebServerResponse* AsyncWebServerRequest::beginResponse(int code,const char* contentType,const char* content){
  auto* r=new AsyncWebServerResponse(code,contentType);
  if(content) r->_body=content;
  return r;
}

AsyncWebServerResponse* AsyncWebServerRequest::beginResponse(int code,const char* contentType,const String& content){
  auto* r=new AsyncWebServerResponse(code,contentType);
  r->_body=content;
  return r;
}

AsyncWebServerResponse* AsyncWebServerRequest::beginChunkedResponse(const char* contentType,std::function<size_t(uint8_t*,size_t,size_t)> cb){
  auto* r=new AsyncWebServerResponse(200,contentType);
  r->_callback=std::move(cb);
  r->_chunked=true;
  return r;
}

static bool applyHeaders(httpd_req_t* req,const AsyncWebServerResponse* r){
  if(httpd_resp_set_status(req,statusText(r->_code))!=ESP_OK) return false;
  if(r->_contentType.length() && httpd_resp_set_type(req,r->_contentType.c_str())!=ESP_OK) return false;
  for(uint8_t i=0;i<r->_headerCount;++i)
    if(httpd_resp_set_hdr(req,r->_headers[i].name.c_str(),r->_headers[i].value.c_str())!=ESP_OK) return false;
  return true;
}

static void runDisconnect(AsyncWebServerRequest* req){
  req->fireDisconnects();
}

void AsyncWebServerRequest::fireDisconnects(){
  auto callbacks=_disconnectCount;
  for(uint8_t i=0;i<callbacks;++i) if(_disconnect[i]) _disconnect[i]();
  _disconnectCount=0;
}

void AsyncWebServerRequest::reportSend(const char* label,esp_err_t err,int savedErrno,uint32_t startMs,size_t bytes,bool logOk){
  if(err!=ESP_OK) _sendFailed=true;
  if(err==ESP_OK && !logOk) return;
  int soErr=0; socklen_t soLen=sizeof(soErr);
  const int fd=_req ? httpd_req_to_sockfd(_req) : -1;
  if(err!=ESP_OK && fd>=0) getsockopt(fd,SOL_SOCKET,SO_ERROR,&soErr,&soLen);   // best effort; clears pending error
  Serial.printf("[HTTP] send %s %s err=%d(0x%04x) errno=%d so_error=%d ms=%lu bytes=%u heap=%u largest=%u\n",
                label?label:"?", err==ESP_OK?"ok":"FAILED", (int)err, (unsigned)err,
                err==ESP_OK?0:savedErrno, soErr, (unsigned long)(millis()-startMs), (unsigned)bytes,
                (unsigned)ESP.getFreeHeap(), (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
}

void AsyncWebServerRequest::send(int code,const char* contentType,const char* content){
  AsyncWebServerResponse* r=beginResponse(code,contentType,content);
  send(r);
}

void AsyncWebServerRequest::send(int code,const char* contentType,const String& content){
  AsyncWebServerResponse* r=beginResponse(code,contentType,content);
  send(r);
}

struct ChunkWorkerCtx {
  httpd_req_t* req=nullptr;
  AsyncWebServerResponse* response=nullptr;
};

static void chunkWorker(void* arg){
  auto* ctx=static_cast<ChunkWorkerCtx*>(arg);
  httpd_req_t* req=ctx->req;
  AsyncWebServerResponse* r=ctx->response;
  bool ok=true;

  if(!applyHeaders(req,r)) ok=false;

  uint8_t buffer[1024];
  size_t index=0;
  unsigned long lastProgress=millis();
  while(ok){
    size_t n=r->_callback ? r->_callback(buffer,sizeof(buffer),index) : 0;
    if(n==(size_t)RESPONSE_TRY_AGAIN){
      if(millis()-lastProgress > 35000UL){ ok=false; break; }
      vTaskDelay(pdMS_TO_TICKS(5));
      continue;
    }
    if(n==0) break;
    lastProgress=millis();
    if(httpd_resp_send_chunk(req,(const char*)buffer,n)!=ESP_OK){ ok=false; break; }
    index+=n;
  }

  if(ok) httpd_resp_send_chunk(req,nullptr,0);
  for(uint8_t i=0;i<r->_disconnectCount;++i) if(r->_disconnect[i]) r->_disconnect[i]();
  r->_disconnectCount=0;
  delete r;
  httpd_req_async_handler_complete(req);
  delete ctx;
  vTaskDelete(nullptr);
}

void AsyncWebServerRequest::send(AsyncWebServerResponse* response){
  if(!response || !_req) { delete response; return; }

  if(response->_chunked){
    httpd_req_t* asyncReq=nullptr;
    if(httpd_req_async_handler_begin(_req,&asyncReq)!=ESP_OK){
      delete response; _sendFailed=true; return;   // nothing sent: close instead of leaving client hanging
    }
    auto* ctx=new ChunkWorkerCtx();
    ctx->req=asyncReq; ctx->response=response;
    response->_disconnectCount=this->_disconnectCount;
    for(uint8_t i=0;i<_disconnectCount;++i) response->_disconnect[i]=_disconnect[i];
    _disconnectCount=0;
    BaseType_t ok=xTaskCreatePinnedToCore(chunkWorker,"nativeHttpChunk",6144,ctx,3,nullptr,0);
    if(ok!=pdPASS){
      delete ctx;
      delete response;
      httpd_req_async_handler_complete(asyncReq);
    }
    return;
  }

  if(!applyHeaders(_req,response)){
    Serial.printf("[HTTP] send headers FAILED (nothing sent) code=%d heap=%u\n",response->_code,(unsigned)ESP.getFreeHeap());
    delete response;
    _sendFailed=true;   // no bytes were sent; closing is the only way the client learns of the failure
    fireDisconnects();
    return;
  }
  const uint32_t t0=millis();
  const size_t nbytes=response->_data ? response->_dataLen : response->_body.length();

  esp_err_t err;
  if(response->_data){
    err=httpd_resp_send(_req,(const char*)response->_data,(ssize_t)response->_dataLen);
  }else{
    err=httpd_resp_send(_req,response->_body.c_str(),(ssize_t)response->_body.length());
  }
  const int savedErrno=errno;
  reportSend("resp",err,savedErrno,t0,nbytes,false);
  delete response;
  fireDisconnects();
}

AsyncWebServer::AsyncWebServer(uint16_t port){
  (void)port;
}

static httpd_method_t nativeMethod(WebRequestMethodComposite method){
  return method;
}
esp_err_t AsyncWebServer::dispatch(httpd_req_t* req){
  if(!req || !req->user_ctx) return ESP_FAIL;
  auto* route=static_cast<Route*>(req->user_ctx);
  if(route->handler){
    AsyncWebServerRequest request(req);
    route->handler(&request);
    // A failed or never-started response must not leave a keep-alive socket open with a
    // truncated body; returning ESP_FAIL makes esp_http_server close this session.
    if(request.sendFailed()) return ESP_FAIL;
  }
  return ESP_OK;
}

esp_err_t AsyncWebServer::registerRoute(Route& route){
  route.native.uri=route.uri.c_str();
  route.native.method=nativeMethod(route.method);
  route.native.handler=&AsyncWebServer::dispatch;
  route.native.user_ctx=&route;
  return httpd_register_uri_handler(_server,&route.native);
}

bool AsyncWebServer::on(const char* uri,WebRequestMethodComposite method,std::function<void(AsyncWebServerRequest*)> handler){
  if(!uri || !handler || _routeCount>=48) return false;
  Route& route=_routes[_routeCount++];
  route.uri=uri;
  route.method=method;
  route.handler=std::move(handler);
  if(_server) return registerRoute(route)==ESP_OK;
  return true;
}

bool AsyncWebServer::begin(){
  if(_server) return true;
  httpd_config_t cfg=HTTPD_DEFAULT_CONFIG();
  cfg.server_port=80;
  cfg.max_uri_handlers=48;
  cfg.max_open_sockets=7;
  cfg.stack_size=8192;
  cfg.task_priority=4;
  cfg.lru_purge_enable=true;
  if(httpd_start(&_server,&cfg)!=ESP_OK){ _server=nullptr; return false; }

  for(size_t i=0;i<_routeCount;++i){
    if(registerRoute(_routes[i])!=ESP_OK){
      Serial.printf("[HTTP] native route registration failed: %s\n",_routes[i].uri.c_str());
    }
  }
  Serial.printf("[HTTP] native esp_http_server started: routes=%u\n",(unsigned)_routeCount);
  return true;
}

void AsyncWebServer::end(){
  if(_server){ httpd_stop(_server); _server=nullptr; }
}