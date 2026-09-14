// smoke.cpp — minimal SpiderMonkey (mozjs-155) embedding smoke test.
//
// Initializes the engine, creates a global/realm, and evaluates a hot JS loop
// (which exercises the Baseline/Ion JIT), then reports the numeric result. Built
// for arm-uwp and linked against dist/lib/mozjs-155.lib; proves the ported
// engine actually executes JavaScript with JIT on a Windows 10 Mobile device.
#include "jsapi.h"

#include "js/CompilationAndEvaluation.h"
#include "js/Context.h"
#include "js/Conversions.h"
#include "js/GlobalObject.h"
#include "js/Initialization.h"
#include "js/Realm.h"
#include "js/RealmOptions.h"
#include "js/SourceText.h"

#include <cstring>
#include <string>

static const JSClass kGlobalClass = {"global", JSCLASS_GLOBAL_FLAGS,
                                     &JS::DefaultGlobalClassOps};

// Returns true on success; fills *out with the JS result and *detail with a
// human-readable status line.
bool RunJsSmoke(int32_t* out, std::string* detail) {
  if (!JS_Init()) {
    *detail = "JS_Init failed";
    return false;
  }

  JSContext* cx = JS_NewContext(JS::DefaultHeapMaxBytes);
  if (!cx) {
    *detail = "JS_NewContext failed";
    return false;
  }

  if (!JS::InitSelfHostedCode(cx)) {
    *detail = "JS::InitSelfHostedCode failed";
    return false;
  }

  JS::RealmOptions options;
  JS::RootedObject global(
      cx, JS_NewGlobalObject(cx, &kGlobalClass, nullptr,
                             JS::FireOnNewGlobalHook, options));
  if (!global) {
    *detail = "JS_NewGlobalObject failed";
    return false;
  }

  bool ok = false;
  {
    JSAutoRealm ar(cx, global);

    // A long integer loop so the methodjit/Ion tiers kick in.
    static const char kCode[] =
        "var s = 0;"
        "for (var i = 0; i < 3000000; i++) {"
        "  s = (s + i * 7 + ((i * i) % 13)) >>> 0;"
        "}"
        "s % 1000000;";

    JS::CompileOptions opts(cx);
    opts.setFileAndLine("smoke.js", 1);

    JS::SourceText<mozilla::Utf8Unit> src;
    if (!src.init(cx, kCode, std::strlen(kCode), JS::SourceOwnership::Borrowed)) {
      *detail = "SourceText::init failed";
    } else {
      JS::RootedValue rval(cx);
      if (!JS::Evaluate(cx, opts, src, &rval)) {
        *detail = "JS::Evaluate failed";
      } else if (!JS::ToInt32(cx, rval, out)) {
        *detail = "JS::ToInt32 failed";
      } else {
        *detail = "JS evaluated with JIT";
        ok = true;
      }
    }
  }

  JS_DestroyContext(cx);
  JS_ShutDown();
  return ok;
}
