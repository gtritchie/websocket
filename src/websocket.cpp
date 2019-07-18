#include <Rcpp.h>

#ifdef _WIN32
// Taken from http://tolstoy.newcastle.edu.au/R/e2/devel/06/11/1242.html
// Undefine the Realloc macro, which is defined by both R and by Windows stuff
#undef Realloc
// Also need to undefine the Free macro
#undef Free

#include <winsock2.h>
#include <windows.h>

// These are defined by windows.h but we have
// to undefine them so that the typedef enum Rboolean will be used later on.
#undef TRUE
#undef FALSE

#endif // _WIN32

#define ASIO_STANDALONE
#include <iostream>
#include <websocketpp/common/thread.hpp>
#include <websocketpp/common/functional.hpp>
#include "client.hpp"

#include <later_api.h>

using namespace Rcpp;

// The websocketpp/common/functional.hpp file detects if a C++11 compiler is
// used. If so, ws_websocketpp::lib::shared_ptr is a std::shared_ptr. If not,
// ws_websocketpp::lib::shared_ptr is a boost::shared_ptr.
using ws_websocketpp::lib::shared_ptr;
using ws_websocketpp::lib::weak_ptr;
using ws_websocketpp::lib::make_shared;

using ws_websocketpp::lib::placeholders::_1;
using ws_websocketpp::lib::placeholders::_2;
using ws_websocketpp::lib::bind;

#include <websocketpp/common/connection_hdl.hpp>

typedef shared_ptr<asio::ssl::context> context_ptr;

#include <boost/function.hpp>
#include <later_api.h>

class Callback {
public:
  virtual ~Callback() {};
  virtual void operator()() = 0;
};

// Wrapper class for lib functions
class LibFunctionCallback : public Callback {
private:
  ws_websocketpp::lib::function<void(void)> fun;

public:
  LibFunctionCallback(ws_websocketpp::lib::function<void(void)> fun)
    : fun(fun) {
  }

  void operator()() {
    fun();
  }
};

// If the Callback class were integrated into later, this wouldn't be
// necessary -- later could accept a void(Callback*) function.
void invoke_open(void* data);

// Invoke a callback and delete the object. The Callback object must have been
// heap-allocated.
void invoke_callback(void* f) {
  Callback* cb = reinterpret_cast<Callback*>(f);
  (*cb)();
  delete cb;
}

// Schedule a function<void(void)> to be invoked with later().
/*void invoke_open(ws_websocketpp::lib::function<void(ws_websocketpp::connection_hdl)> f,
                 ws_websocketpp::connection_hdl hdl) {
  ws_websocketpp::lib::function<void(void)> wrappedFun = bind(f, hdl);
  LibFunctionCallback* b_fun = new LibFunctionCallback(wrappedFun);
  later::later(invoke_callback, b_fun, 0.0);
                 }*/

static context_ptr on_tls_init() {
  context_ptr ctx = make_shared<asio::ssl::context>(asio::ssl::context::sslv23);
  try {
    ctx->set_options(asio::ssl::context::default_workarounds |
      asio::ssl::context::no_sslv2 |
      asio::ssl::context::no_sslv3 |
      asio::ssl::context::single_dh_use);
  } catch (std::exception &e) {
    Rcpp::Rcout << "Error in context pointer: " << e.what() << std::endl;
  }
  return ctx;
}


class WSConnection {
public:
  WSConnection(shared_ptr<Client> client, Rcpp::Environment robjPublic, Rcpp::Environment robjPrivate) :
  client(client), robjPublic(robjPublic), robjPrivate(robjPrivate) {}

  enum STATE { INIT, OPEN, CLOSING, CLOSED, FAILED };
  STATE state = INIT;
  shared_ptr<Client> client;

  Rcpp::Environment robjPublic;
  Rcpp::Environment robjPrivate;

  Rcpp::Function getInvoker(std::string name) {
    Rcpp::Function gi = robjPrivate.get("getInvoker");
    return gi(name);
  }

  bool closeOnOpen = false;
};


shared_ptr<WSConnection> xptrGetClient(SEXP client_xptr) {
  if (TYPEOF(client_xptr) != EXTPTRSXP) {
    throw Rcpp::exception("Expected external pointer.");
  }
  return *reinterpret_cast<shared_ptr<WSConnection>*>(R_ExternalPtrAddr(client_xptr));
}

void client_deleter(SEXP client_xptr) {
  delete reinterpret_cast<shared_ptr<WSConnection>*>(R_ExternalPtrAddr(client_xptr));
  R_ClearExternalPtr(client_xptr);
}

void handleMessageCB(shared_ptr<WSConnection> wsPtr, message_ptr msg){
  Rcpp::Rcout << "Message callback" << std::endl;

  ws_websocketpp::frame::opcode::value opcode = msg->get_opcode();

  Rcpp::List event;
  event["target"] = wsPtr->robjPublic;
  if (opcode == ws_websocketpp::frame::opcode::value::text) {
    event["data"] = msg->get_payload();

  } else if (opcode == ws_websocketpp::frame::opcode::value::binary) {
    const std::string msg_str = msg->get_payload();
    event["data"] = std::vector<uint8_t>(msg_str.begin(), msg_str.end());

  } else {
    stop("Unknown opcode for message (not text or binary).");
  }

  wsPtr->getInvoker("message")(event);
}
void handleMessage(weak_ptr<WSConnection> wsPtrWeak, ws_websocketpp::connection_hdl, message_ptr msg) {
  shared_ptr<WSConnection> wsPtr = wsPtrWeak.lock();
  if (wsPtr) {

    Rcpp::Rcout << "Handle message later" << std::endl;
    ws_websocketpp::lib::function<void(void)> wrappedFun = bind(handleMessageCB, wsPtr, msg);
    LibFunctionCallback* b_fun = new LibFunctionCallback(wrappedFun);
    later::later(invoke_callback, b_fun, 0.0);
  }
}

void removeHandlers(shared_ptr<WSConnection> wsPtr) {
  wsPtr->robjPublic = Rcpp::Environment();
  wsPtr->robjPrivate = Rcpp::Environment();
}

void handleCloseCB(shared_ptr<WSConnection> wsPtr){
  Rcpp::Rcout << "Close callback" << std::endl;
  Rcpp::List event;

  ws_websocketpp::close::status::value code = wsPtr->client->get_remote_close_code();
  std::string reason = wsPtr->client->get_remote_close_reason();

  wsPtr->state = WSConnection::STATE::CLOSED;
  Rcpp::Function onClose = wsPtr->getInvoker("close");

  event["target"] = wsPtr->robjPublic;
  event["code"] = code;
  event["reason"] = reason;

  removeHandlers(wsPtr);
  onClose(event);
}
void handleClose(weak_ptr<WSConnection> wsPtrWeak, ws_websocketpp::connection_hdl) {
  shared_ptr<WSConnection> wsPtr = wsPtrWeak.lock();
  if (wsPtr) {
    Rcpp::Rcout << "Close callback later" << std::endl;
    ws_websocketpp::lib::function<void(void)> wrappedFun = bind(handleCloseCB, wsPtr);
    LibFunctionCallback* b_fun = new LibFunctionCallback(wrappedFun);
    later::later(invoke_callback, b_fun, 0.0);
  }
}

void handleOpenCB(shared_ptr<WSConnection> wsPtr){
  Rcpp::Rcout << "Open callback" << std::endl;
  Rcpp::List event;
  event["target"] = wsPtr->robjPublic;
  wsPtr->getInvoker("open")(event);
}
void handleOpen(weak_ptr<WSConnection> wsPtrWeak, ws_websocketpp::connection_hdl) {
  shared_ptr<WSConnection> wsPtr = wsPtrWeak.lock();
  if (wsPtr) {
    if (wsPtr->closeOnOpen) {
      wsPtr->state = WSConnection::STATE::CLOSING;
      wsPtr->client->close(ws_websocketpp::close::status::normal, "");
      return;
    }

    wsPtr->state = WSConnection::STATE::OPEN;

    Rcpp::Rcout << "Open callback later" << std::endl;
    ws_websocketpp::lib::function<void(void)> wrappedFun = bind(handleOpenCB, wsPtr);
    LibFunctionCallback* b_fun = new LibFunctionCallback(wrappedFun);
    later::later(invoke_callback, b_fun, 0.0);
  }
}

void handleFailCB(shared_ptr<WSConnection> wsPtr, String errMessage){
  Rcpp::Rcout << "Fail " << std::endl;
  Rcpp::List event;
  event["target"] = wsPtr->robjPublic;
  event["message"] = errMessage;

  Rcpp::Function onFail = wsPtr->getInvoker("error");
  Rcpp::Rcout << "Running onfail" << std::endl;
  removeHandlers(wsPtr);
  onFail(event);
  Rcpp::Rcout << "Exiting fail callback" << std::endl;
}
void handleFail(weak_ptr<WSConnection> wsPtrWeak, ws_websocketpp::connection_hdl) {
  shared_ptr<WSConnection> wsPtr = wsPtrWeak.lock();
  if (wsPtr) {
    wsPtr->state = WSConnection::STATE::FAILED;
    ws_websocketpp::lib::error_code ec = wsPtr->client->get_ec();

    Rcpp::Rcout << "Scheduling fail callback" << std::endl;
    ws_websocketpp::lib::function<void(void)> wrappedFun = bind(handleFailCB, wsPtr, ec.message());
    LibFunctionCallback* b_fun = new LibFunctionCallback(wrappedFun);
    later::later(invoke_callback, b_fun, 0.0);
  }
}

// [[Rcpp::export]]
SEXP wsCreate(
  std::string uri,
  Rcpp::Environment robjPublic,
  Rcpp::Environment robjPrivate,
  Rcpp::CharacterVector accessLogChannels,
  Rcpp::CharacterVector errorLogChannels
) {
  if (uri.size() < 6) {
    throw Rcpp::exception("Invalid websocket URI: too short");
  }

  shared_ptr<WSConnection> wsPtr;

  if (uri.substr(0, 5) == "ws://") {
    shared_ptr<ClientImpl<ws_client>> client = make_shared<ClientImpl<ws_client>>();
    wsPtr = make_shared<WSConnection>(client, robjPublic, robjPrivate);

  } else if (uri.substr(0, 6) == "wss://") {
    shared_ptr<ClientImpl<wss_client>> client = make_shared<ClientImpl<wss_client>>();
    wsPtr = make_shared<WSConnection>(client, robjPublic, robjPrivate);
    wsPtr->client->set_tls_init_handler(bind(&on_tls_init));

  } else {
    throw Rcpp::exception("Invalid websocket URI: must begin with ws:// or wss://");
  }

  weak_ptr<WSConnection> wsPtrWeak(wsPtr);

  if (accessLogChannels.size() > 0) {
    // clear all channels and set user channels
    wsPtr->client->clear_access_channels(ws_websocketpp::log::alevel::all);
    wsPtr->client->update_log_channels("access", "set", accessLogChannels);
  }
  if (errorLogChannels.size() > 0) {
    // clear all channels and set user channels
    wsPtr->client->clear_error_channels(ws_websocketpp::log::elevel::all);
    wsPtr->client->update_log_channels("error", "set", errorLogChannels);
  }
  wsPtr->client->init_asio();
  wsPtr->client->set_open_handler(bind(handleOpen, wsPtrWeak, ::_1));
  wsPtr->client->set_message_handler(bind(handleMessage, wsPtrWeak, ::_1, ::_2));
  wsPtr->client->set_close_handler(bind(handleClose, wsPtrWeak, ::_1));
  wsPtr->client->set_fail_handler(bind(handleFail, wsPtrWeak, ::_1));

  ws_websocketpp::lib::error_code ec;
  wsPtr->client->setup_connection(uri, ec);
  if (ec) {
    // TODO Should we call onFail here?
    stop("Could not create connection because: " + ec.message());
  }

  shared_ptr<WSConnection> *extwsPtr = new shared_ptr<WSConnection>(wsPtr);
  SEXP client_xptr = PROTECT(R_MakeExternalPtr(extwsPtr, R_NilValue, R_NilValue));
  R_RegisterCFinalizerEx(client_xptr, client_deleter, TRUE);
  UNPROTECT(1);
  return client_xptr;
}

// [[Rcpp::export]]
void wsAppendHeader(SEXP client_xptr, std::string key, std::string value) {
  shared_ptr<WSConnection> wsPtr = xptrGetClient(client_xptr);
  wsPtr->client->append_header(key, value);
}

// [[Rcpp::export]]
void wsAddProtocols(SEXP client_xptr, CharacterVector protocols) {
  shared_ptr<WSConnection> wsPtr = xptrGetClient(client_xptr);
  for (Rcpp::CharacterVector::iterator it = protocols.begin();
       it != protocols.end();
       it++) {
    std::string protocol = Rcpp::as<std::string>(*it);
    wsPtr->client->add_subprotocol(protocol);
  }
}

void doRun(shared_ptr<WSConnection> wsPtr){
  Rcpp::Rcout << "Do run" << std::endl;
  wsPtr->client->run();
  Rcpp::Rcout << "Done running run" << std::endl;
}

// [[Rcpp::export]]
void wsConnect(SEXP client_xptr, Function run_now) {
  shared_ptr<WSConnection> wsPtr = xptrGetClient(client_xptr);
  wsPtr->client->connect();

  /* FIXME: Do we need this equivalent here:
   * if (self$readyState() == 3L) {
        return()
     }
   */

  ws_websocketpp::lib::thread t(doRun, wsPtr);

  // Keep the thread running even thought it's about to go out of scope.
  t.detach();

  // The docs aren't clear, but I believe that connect() can -- at least for
  // some implementations -- be asynchronous. If we return and let the client
  // e.g. call a send() before the connection is open, things go poorly. Since
  // we expect this to be a quick operation, let's just let spin here until the
  // ambiguity is resolved.
  // TODO: there's surely a cleaner way to do this.

  while (wsPtr->state == WSConnection::STATE::INIT){
    // TOOD: add a timeout
    run_now(-1);
  }

}

// [[Rcpp::export]]
void wsRestart(SEXP client_xptr) {
  shared_ptr<WSConnection> wsPtr = xptrGetClient(client_xptr);
  wsPtr->client->get_io_service().restart();
}

// [[Rcpp::export]]
void wsPoll(SEXP client_xptr) {
  shared_ptr<WSConnection> wsPtr = xptrGetClient(client_xptr);
  wsPtr->client->poll();
}

// [[Rcpp::export]]
void wsSend(SEXP client_xptr, SEXP msg) {
  shared_ptr<WSConnection> wsPtr = xptrGetClient(client_xptr);

  if (TYPEOF(msg) == STRSXP &&
      Rf_length(msg) == 1 &&
      STRING_ELT(msg, 0) != NA_STRING)
  {
    const char* msg_ptr = CHAR(STRING_ELT(msg, 0));
    int len = R_nchar(STRING_ELT(msg, 0), Bytes, FALSE, FALSE, "wsSend");
    wsPtr->client->send(msg_ptr, len, ws_websocketpp::frame::opcode::text);
  } else if (TYPEOF(msg) == RAWSXP) {
    wsPtr->client->send(RAW(msg), Rf_length(msg), ws_websocketpp::frame::opcode::binary);
  } else {
    stop("msg must be a one-element character vector or a raw vector.");
  }
}

// [[Rcpp::export]]
void wsReset(SEXP client_xptr) {
  shared_ptr<WSConnection> wsPtr = xptrGetClient(client_xptr);
  wsPtr->client->reset();
}

// [[Rcpp::export]]
void wsClose(SEXP client_xptr, uint16_t code, std::string reason) {
  shared_ptr<WSConnection> wsPtr = xptrGetClient(client_xptr);

  switch (wsPtr->state) {
  case WSConnection::STATE::INIT:
    wsPtr->closeOnOpen = true;
    return;
  case WSConnection::STATE::OPEN:
    break;
  case WSConnection::STATE::CLOSING:
  case WSConnection::STATE::CLOSED:
  case WSConnection::STATE::FAILED:
    return;
  }

  wsPtr->state = WSConnection::STATE::CLOSING;
  wsPtr->client->close(code, reason);
}

// [[Rcpp::export]]
bool wsStopped(SEXP client_xptr) {
  shared_ptr<WSConnection> wsPtr = xptrGetClient(client_xptr);
  return wsPtr->client->stopped();
}

// [[Rcpp::export]]
std::string wsProtocol(SEXP client_xptr) {
  shared_ptr<WSConnection> wsPtr = xptrGetClient(client_xptr);
  return wsPtr->client->get_subprotocol();
}

// [[Rcpp::export]]
std::string wsState(SEXP client_xptr) {
  shared_ptr<WSConnection> wsPtr = xptrGetClient(client_xptr);
  switch(wsPtr->state) {
    case WSConnection::STATE::INIT: return "INIT";
    case WSConnection::STATE::OPEN: return "OPEN";
    case WSConnection::STATE::CLOSING: return "CLOSING";
    case WSConnection::STATE::CLOSED: return "CLOSED";
    case WSConnection::STATE::FAILED: return "FAILED";
  }

  // Shouldn't be possible to get here, but some compilers still complain
  // about reaching end of a non-void function.
  return "UNKNOWN";
}

// [[Rcpp::export]]
void wsUpdateLogChannels(
  SEXP client_xptr,
  std::string accessOrError,
  std::string setOrClear,
  Rcpp::CharacterVector logChannels
) {
  shared_ptr<WSConnection> wsPtr = xptrGetClient(client_xptr);
  wsPtr->client->update_log_channels(accessOrError, setOrClear, logChannels);
}
