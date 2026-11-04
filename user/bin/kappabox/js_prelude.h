/*
 * js_prelude.h: the part of the `js` applet that is written in JavaScript: util.inspect (the text console.log and the
 * prompt show for a value), the console object, and the format() behind console.log's %s %d %j.
 *
 * It is JavaScript because the inspector asks a hundred questions of a value (what class is it, which keys, which
 * accessors, is it a Map ...) that the language answers directly and the C API only through a call each.  The text is
 * stringified from the macro argument below, which has three consequences for the code in it: no // comments (the
 * newlines are gone), no regular expression literals and no apostrophes or backticks outside string literals (the C
 * preprocessor would read them as the start of a character constant), so it uses block comments and double quotes.
 * tests/host/jstool.c includes this file too (jstool --console), which is how tests/js/inspect.js checks the output.
 *
 * The prelude is a function expression; the host calls it with an object of natives:
 *   host.write(fd, text)            write to standard output (1) or standard error (2)
 *   host.now()                      a monotonic clock in milliseconds (console.time)
 *   host.promiseState(promise)      [state, result] with state 0 pending, 1 fulfilled, 2 rejected (undefined: not a promise)
 * and it returns { inspect, format }, installing console on the global object.  Following Node's util.inspect, which
 * most programmers know: depth 2, line width 80, groups of numbers in columns, circular references as <ref *1>.
 */
#ifndef KAPPABOX_JS_PRELUDE_H
#define KAPPABOX_JS_PRELUDE_H

#define KB_JS_SOURCE(...) #__VA_ARGS__

static const char kb_js_prelude[] = KB_JS_SOURCE(
(function (host) {
  "use strict";
  var BREAK = 80, COMPACT = 3, MAX_ARRAY = 100, MAX_STRING = 10000;
  var ObjToString = Object.prototype.toString, FnToString = Function.prototype.toString;
  var getProto = Object.getPrototypeOf, getDesc = Object.getOwnPropertyDescriptor;
  var ESC = { 8: "\\b", 9: "\\t", 10: "\\n", 12: "\\f", 13: "\\r" };

  function hex(n, w) { var s = n.toString(16).toUpperCase(); while (s.length < w) s = "0" + s; return s; }
  function pad(s, n, left) { while (s.length < n) s = left ? " " + s : s + " "; return s; }

  function quote(s) {
    var q = "'";
    if (s.indexOf("'") >= 0) {
      if (s.indexOf("\"") < 0) q = "\"";
      else if (s.indexOf("`") < 0 && s.indexOf("${") < 0) q = "`";
    }
    var out = q;
    for (var i = 0; i < s.length; i++) {
      var c = s[i], n = s.charCodeAt(i);
      if (c === q || c === "\\") out += "\\" + c;
      else if (n < 32 || n === 127) out += ESC[n] || "\\x" + hex(n, 2);
      else if (n >= 0xD800 && n <= 0xDBFF && i + 1 < s.length && s.charCodeAt(i + 1) >= 0xDC00 && s.charCodeAt(i + 1) <= 0xDFFF) { out += c + s[i + 1]; i++; }
      else if (n >= 0xD800 && n <= 0xDFFF) out += "\\u" + hex(n, 4);
      else out += c;
    }
    return out + q;
  }

  function isIdent(k) {
    if (k.length === 0) return false;
    for (var i = 0; i < k.length; i++) {
      var c = k.charCodeAt(i);
      var ok = (c >= 65 && c <= 90) || (c >= 97 && c <= 122) || c === 95 || c === 36 || (i > 0 && c >= 48 && c <= 57);
      if (!ok) return false;
    }
    return true;
  }

  function isIndexKey(k) {
    if (typeof k !== "string" || k.length === 0 || k.length > 10) return false;
    for (var i = 0; i < k.length; i++) { var c = k.charCodeAt(i); if (c < 48 || c > 57) return false; }
    return (k === "0" || k[0] !== "0") && Number(k) < 4294967295;
  }

  function tagOf(v) { return ObjToString.call(v).slice(8, -1); }
  function brand(f, v) { try { f(v); return true; } catch (e) { return false; } }
  var mapHas = Map.prototype.has, setHas = Set.prototype.has, wmHas = WeakMap.prototype.has, wsHas = WeakSet.prototype.has;
  var mapEach = Map.prototype.forEach, setEach = Set.prototype.forEach;
  var mapSize = getDesc(Map.prototype, "size").get, setSize = getDesc(Set.prototype, "size").get;
  var dateTime = Date.prototype.getTime, abLen = getDesc(ArrayBuffer.prototype, "byteLength").get;
  var sabLen = typeof SharedArrayBuffer === "function" ? getDesc(SharedArrayBuffer.prototype, "byteLength").get : null;
  var reSource = getDesc(RegExp.prototype, "source").get, dvLen = getDesc(DataView.prototype, "byteLength").get;
  var numOf = Number.prototype.valueOf, strOf = String.prototype.valueOf, boolOf = Boolean.prototype.valueOf;
  var symOf = Symbol.prototype.valueOf, bigOf = BigInt.prototype.valueOf;

  function ctorName(v) {
    var o = v, depth = 0;
    while (o !== null && o !== undefined && depth++ < 1000) {
      var d = getDesc(o, "constructor");
      if (d !== undefined && typeof d.value === "function" && d.value.name !== "") return d.value.name;
      o = getProto(o);
    }
    return null;
  }

  function ownKeys(v, arrayLike) {
    var keys = Object.keys(v);
    if (arrayLike) keys = keys.filter(function (k) { return !isIndexKey(k); });
    var syms = Object.getOwnPropertySymbols(v);
    for (var i = 0; i < syms.length; i++) if (Object.prototype.propertyIsEnumerable.call(v, syms[i])) keys.push(syms[i]);
    return keys;
  }

  function formatNumber(n) { return Object.is(n, -0) ? "-0" : String(n); }

  function formatPrimitive(ctx, v) {
    switch (typeof v) {
    case "string": {
      var s = v, trailer = "";
      if (s.length > MAX_STRING) { trailer = "... " + (s.length - MAX_STRING) + " more character" + (s.length - MAX_STRING > 1 ? "s" : ""); s = s.slice(0, MAX_STRING); }
      if (s.length > 16 && s.length > BREAK - ctx.indent - 4 && s.indexOf("\n") >= 0 && s.indexOf("\n") < s.length - 1) {
        var parts = [], from = 0, at;
        while ((at = s.indexOf("\n", from)) >= 0 && at < s.length - 1) { parts.push(s.slice(from, at + 1)); from = at + 1; }
        parts.push(s.slice(from));
        return parts.map(quote).join(" +\n" + pad("", ctx.indent + 2)) + trailer;
      }
      return quote(s) + trailer;
    }
    case "number": return formatNumber(v);
    case "bigint": return v + "n";
    case "symbol": return v.toString();
    default: return String(v);
    }
  }

  function formatValue(ctx, v, level, top) {
    if (typeof v !== "object" && typeof v !== "function") return formatPrimitive(ctx, v);
    if (v === null) return "null";
    if (ctx.seen.indexOf(v) >= 0) {
      var index = 1;
      if (ctx.circular === null) { ctx.circular = new Map(); ctx.circular.set(v, index); }
      else { index = ctx.circular.get(v); if (index === undefined) { index = ctx.circular.size + 1; ctx.circular.set(v, index); } }
      return "[Circular *" + index + "]";
    }
    return formatRaw(ctx, v, level);
  }

  function errorBase(v) {
    var stack;
    try { stack = v.stack; } catch (e) { stack = undefined; }
    if (typeof stack === "string" && stack !== "") return stack;
    var s;
    try { s = Error.prototype.toString.call(v); } catch (e) { s = "Error"; }
    return "[" + s + "]";
  }

  function formatRaw(ctx, v, level) {
    var ctor = ctorName(v), tag = tagOf(v), keys, base = "", braces, entries = null, kind = 0;
    var protoNull = ctor === null;
    var prefixName = protoNull ? "[" + tag + ": null prototype]" : ctor;
    if (typeof v === "function") {
      var src = FnToString.call(v), name = "";
      try { name = v.name; } catch (e) { name = ""; }
      if (src.slice(0, 5) === "class" && (src.length === 5 || " {\n\t".indexOf(src[5]) >= 0)) {
        base = "[class " + (typeof name === "string" && name !== "" ? name : "(anonymous)");
        var sup = getProto(v);
        if (typeof sup === "function" && sup !== Function.prototype && sup.name) base += " extends " + sup.name;
        base += "]";
      } else {
        var type = tag === "Function" || tag === "Object" ? "Function" : tag;
        base = "[" + type + (typeof name === "string" && name !== "" ? ": " + name : " (anonymous)") + "]";
      }
      keys = ownKeys(v, false);
      braces = ["{", "}"];
      if (keys.length === 0) return base;
    } else if (Array.isArray(v)) {
      keys = ownKeys(v, true);
      braces = [(ctor !== "Array" ? prefixName + "(" + v.length + ") " : "") + "[", "]"];
      kind = 1;
      if (v.length === 0 && keys.length === 0) return braces[0] + "]";
    } else if (tag === "Map" && brand(function (x) { mapHas.call(x, 0); }, v)) {
      keys = ownKeys(v, false);
      var msize = mapSize.call(v);
      braces = [prefixName + "(" + msize + ") {", "}"];
      kind = 2;
      if (msize === 0 && keys.length === 0) return braces[0] + "}";
    } else if (tag === "Set" && brand(function (x) { setHas.call(x, 0); }, v)) {
      keys = ownKeys(v, false);
      var ssize = setSize.call(v);
      braces = [prefixName + "(" + ssize + ") {", "}"];
      kind = 3;
      if (ssize === 0 && keys.length === 0) return braces[0] + "}";
    } else if (ArrayBuffer.isView(v) && tag !== "DataView") {
      keys = ownKeys(v, true);
      braces = [prefixName + "(" + v.length + ") [", "]"];
      kind = 4;
      if (v.length === 0 && keys.length === 0) return braces[0] + "]";
    } else {
      keys = ownKeys(v, false);
      var isObj = true;
      braces = ["{", "}"];
      if (tag === "Arguments" && brand(function (x) { return x.length; }, v)) {
        braces = ["[Arguments] [", "]"]; kind = 1; keys = ownKeys(v, true); isObj = false;
        if (v.length === 0 && keys.length === 0) return "[Arguments] []";
      } else if (tag === "Error" || (v instanceof Error)) {
        base = errorBase(v);
        keys = keys.filter(function (k) { return k !== "stack" && k !== "message"; });
        if (ctx.indent > 0) base = base.split("\n").join("\n" + pad("", ctx.indent));
        if (keys.length === 0) return base;
        isObj = false;
      } else if (tag === "Date" && brand(function (x) { dateTime.call(x); }, v)) {
        var tm = dateTime.call(v);
        base = tm !== tm ? "Invalid Date" : v.toISOString();
        if (keys.length === 0) return base;
        isObj = false;
      } else if (tag === "RegExp" && brand(function (x) { reSource.call(x); }, v) && v !== RegExp.prototype) {
        base = RegExp.prototype.toString.call(v);
        if (keys.length === 0) return base;
        isObj = false;
      } else if (tag === "Promise" && host.promiseState(v) !== undefined) {
        var st = host.promiseState(v);
        braces = [prefixName + " {", "}"];
        kind = 5;
        ctx.promise = st;
        isObj = false;
      } else if ((tag === "WeakMap" && brand(function (x) { wmHas.call(x, {}); }, v)) || (tag === "WeakSet" && brand(function (x) { wsHas.call(x, {}); }, v))) {
        braces = [prefixName + " {", "}"]; kind = 6; isObj = false;
      } else if (tag === "ArrayBuffer" && brand(function (x) { abLen.call(x); }, v)) {
        braces = [prefixName + " {", "}"]; kind = 7; isObj = false;
      } else if (tag === "SharedArrayBuffer" && sabLen !== null && brand(function (x) { sabLen.call(x); }, v)) {
        braces = [prefixName + " {", "}"]; kind = 7; isObj = false;
      } else if (tag === "DataView" && brand(function (x) { dvLen.call(x); }, v)) {
        braces = [prefixName + " {", "}"]; kind = 8; isObj = false;
      } else if (tag === "Number" && brand(function (x) { numOf.call(x); }, v)) {
        base = "[Number: " + formatNumber(numOf.call(v)) + "]"; isObj = false;
        if (keys.length === 0) return base;
      } else if (tag === "String" && brand(function (x) { strOf.call(x); }, v)) {
        base = "[String: " + quote(strOf.call(v)) + "]"; isObj = false;
        keys = keys.filter(function (k) { return !isIndexKey(k); });
        if (keys.length === 0) return base;
      } else if (tag === "Boolean" && brand(function (x) { boolOf.call(x); }, v)) {
        base = "[Boolean: " + boolOf.call(v) + "]"; isObj = false;
        if (keys.length === 0) return base;
      } else if (tag === "Symbol" && brand(function (x) { symOf.call(x); }, v)) {
        base = "[Symbol: " + symOf.call(v).toString() + "]"; isObj = false;
        if (keys.length === 0) return base;
      } else if (tag === "BigInt" && brand(function (x) { bigOf.call(x); }, v)) {
        base = "[BigInt: " + bigOf.call(v) + "n]"; isObj = false;
        if (keys.length === 0) return base;
      }
      if (isObj) {
        var shown = protoNull ? "[Object: null prototype]" : ctor;
        if (tag !== "Object" && tag !== ctor && !protoNull) shown = (ctor === null ? "Object" : ctor) + " [" + tag + "]";
        else if (protoNull && tag !== "Object") shown = "[" + tag + ": null prototype]";
        if (shown !== "Object") braces[0] = shown + " {";
        if (keys.length === 0) return braces[0] + "}";
      }
    }
    if (level > ctx.depth) {
      if (kind === 1 || kind === 4) return "[" + (ctor === null ? tag : ctor) + "]";
      return "[" + (ctor === null ? "Object: null prototype" : ctor) + "]";
    }
    ctx.seen.push(v);
    ctx.currentDepth = level;
    var output = [], i;
    var promiseState = ctx.promise;
    ctx.promise = null;
    try {
      if (kind === 1) {
        var len = v.length, shownLen = Math.min(len, MAX_ARRAY), holes = 0;
        for (i = 0; i < shownLen; i++) {
          if (!Object.prototype.hasOwnProperty.call(v, i)) {
            holes = 1;
            while (i + 1 < len && !Object.prototype.hasOwnProperty.call(v, i + 1)) { i++; holes++; }
            output.push("<" + holes + " empty item" + (holes > 1 ? "s" : "") + ">");
            continue;
          }
          output.push(formatProperty(ctx, v, level, i, 1));
        }
        if (len > i && i >= MAX_ARRAY) output.push("... " + (len - i) + " more item" + (len - i > 1 ? "s" : ""));
      } else if (kind === 4) {
        var tl = v.length, ts = Math.min(tl, MAX_ARRAY);
        for (i = 0; i < ts; i++) output.push(typeof v[i] === "bigint" ? v[i] + "n" : formatNumber(v[i]));
        if (tl > ts) output.push("... " + (tl - ts) + " more item" + (tl - ts > 1 ? "s" : ""));
      } else if (kind === 2) {
        ctx.indent += 2;
        mapEach.call(v, function (val, key) { output.push(formatValue(ctx, key, level + 1) + " => " + formatValue(ctx, val, level + 1)); });
        ctx.indent -= 2;
      } else if (kind === 3) {
        ctx.indent += 2;
        setEach.call(v, function (val) { output.push(formatValue(ctx, val, level + 1)); });
        ctx.indent -= 2;
      } else if (kind === 5) {
        if (promiseState[0] === 0) output.push("<pending>");
        else {
          ctx.indent += 2;
          var inner = formatValue(ctx, promiseState[1], level + 1);
          ctx.indent -= 2;
          output.push(promiseState[0] === 2 ? "<rejected> " + inner : inner);
        }
      } else if (kind === 6) {
        output.push("<items unknown>");
      } else if (kind === 7) {
        var bytes = new Uint8Array(v), bl = bytes.length, parts = [];
        for (i = 0; i < Math.min(bl, 50); i++) parts.push(hex(bytes[i], 2).toLowerCase());
        output.push("[Uint8Contents]: <" + parts.join(" ") + (bl > 50 ? " ... " + (bl - 50) + " more byte" + (bl - 50 > 1 ? "s" : "") : "") + ">");
        output.push("byteLength: " + bl);
      } else if (kind === 8) {
        ctx.indent += 2;
        output.push("byteLength: " + v.byteLength, "byteOffset: " + v.byteOffset, "buffer: " + formatValue(ctx, v.buffer, level + 1));
        ctx.indent -= 2;
      }
      for (i = 0; i < keys.length; i++) output.push(formatProperty(ctx, v, level, keys[i], 0));
    } finally {
      ctx.seen.pop();
    }
    if (ctx.circular !== null) {
      var ref = ctx.circular.get(v);
      if (ref !== undefined) {
        var label = "<ref *" + ref + ">";
        if (base !== "") base = label + " " + base;
        else braces[0] = label + " " + braces[0];
      }
    }
    return reduceToSingleString(ctx, output, base, braces, kind === 1 || kind === 4, level, v);
  }

  function formatProperty(ctx, obj, level, key, type) {
    var desc = getDesc(obj, key) || { value: obj[key], enumerable: true }, str, name;
    if (desc.value !== undefined || (!desc.get && !desc.set)) {
      ctx.indent += 2;
      str = formatValue(ctx, desc.value, level + 1);
      ctx.indent -= 2;
    } else if (desc.get !== undefined) str = desc.set !== undefined ? "[Getter/Setter]" : "[Getter]";
    else if (desc.set !== undefined) str = "[Setter]";
    else str = "undefined";
    if (type === 1) return str;
    if (typeof key === "symbol") name = "[" + key.toString() + "]";
    else if (isIdent(key)) name = key;
    else name = quote(key);
    if (type === 0 && desc.enumerable === false) name = "[" + name + "]";
    return name + ": " + str;
  }

  function groupArrayElements(ctx, output, value) {
    var totalLength = 0, maxLength = 0, i = 0, outputLength = output.length;
    if (value !== undefined && output.length > MAX_ARRAY && output[outputLength - 1].slice(0, 4) === "... ") outputLength--;
    var separatorSpace = 2, dataLen = new Array(outputLength);
    for (; i < outputLength; i++) {
      var len = output[i].length;
      dataLen[i] = len;
      totalLength += len + separatorSpace;
      if (maxLength < len) maxLength = len;
    }
    var actualMax = maxLength + separatorSpace;
    if (actualMax * 3 + ctx.indent < BREAK && (totalLength / actualMax > 5 || maxLength <= 6)) {
      var averageBias = Math.sqrt(actualMax - totalLength / output.length);
      var biasedMax = Math.max(actualMax - 3 - averageBias, 1);
      var columns = Math.min(Math.round(Math.sqrt(2.5 * biasedMax * outputLength) / biasedMax), Math.floor((BREAK - ctx.indent) / actualMax), COMPACT * 4, 15);
      if (columns <= 1) return output;
      var tmp = [], maxLineLength = [];
      for (i = 0; i < columns; i++) {
        var lineLength = 0;
        for (var j = i; j < output.length; j += columns) if (dataLen[j] > lineLength) lineLength = dataLen[j];
        maxLineLength.push(lineLength + separatorSpace);
      }
      var left = true;
      if (value !== undefined) {
        for (i = 0; i < output.length; i++) if (typeof value[i] !== "number" && typeof value[i] !== "bigint") { left = false; break; }
      }
      for (i = 0; i < outputLength; i += columns) {
        var max = Math.min(i + columns, outputLength), str = "", k = i;
        for (; k < max - 1; k++) str += pad(output[k] + ", ", maxLineLength[k - i], left);
        if (left) str += pad(output[k], maxLineLength[k - i] - separatorSpace, true);
        else str += output[k];
        tmp.push(str);
      }
      if (outputLength < output.length) tmp.push(output[outputLength]);
      output = tmp;
    }
    return output;
  }

  function isBelowBreakLength(ctx, output, start, base) {
    var total = output.length + start;
    if (total + output.length > BREAK) return false;
    for (var i = 0; i < output.length; i++) {
      total += output[i].length;
      if (total > BREAK) return false;
    }
    return base === "" || base.indexOf("\n") < 0;
  }

  function reduceToSingleString(ctx, output, base, braces, arrayLike, level, value) {
    var entries = output.length;
    if (arrayLike && entries > 6) output = groupArrayElements(ctx, output, value);
    if (ctx.currentDepth - level < COMPACT && entries === output.length) {
      var start = output.length + ctx.indent + braces[0].length + base.length + 10;
      if (isBelowBreakLength(ctx, output, start, base)) {
        var joined = output.join(", ");
        if (joined.indexOf("\n") < 0) return (base !== "" ? base + " " : "") + braces[0] + " " + joined + " " + braces[1];
      }
    }
    var indentation = "\n" + pad("", ctx.indent);
    return (base !== "" ? base + " " : "") + braces[0] + indentation + "  " + output.join("," + indentation + "  ") + indentation + braces[1];
  }

  function inspect(v, opts) {
    var ctx = { seen: [], circular: null, indent: 0, currentDepth: 0, depth: 2, promise: null };
    if (opts !== null && typeof opts === "object") {
      if (opts.depth === null || opts.depth === Infinity) ctx.depth = Infinity;
      else if (typeof opts.depth === "number") ctx.depth = opts.depth;
    }
    return formatValue(ctx, v, 0);
  }

  /* %s prints an object with String() only when it brings its own toString; the built-in ones are inspected */
  function builtinToString(o) {
    try {
      var f = o.toString;
      return typeof f !== "function" || FnToString.call(f).indexOf("[native code]") >= 0;
    } catch (e) { return true; }
  }

  function formatWithOptions(args) {
    var i = 0, out = "", first = args[0];
    if (typeof first === "string") {
      i = 1;
      if (args.length === 1) return first;
      var last = 0, p = 0;
      for (var at = 0; at < first.length - 1; at++) {
        if (first[at] !== "%") continue;
        var c = first[at + 1], piece;
        if (c === "%") { out += first.slice(last, at) + "%"; last = at + 2; at++; continue; }
        if (i >= args.length) continue;
        var a = args[i];
        switch (c) {
        case "s":
          if (typeof a === "number") piece = formatNumber(a);
          else if (typeof a === "bigint") piece = a + "n";
          else if (typeof a !== "object" || a === null || !builtinToString(a)) piece = String(a);
          else piece = inspect(a, { depth: 0 });
          break;
        case "d":
          piece = typeof a === "bigint" ? a + "n" : typeof a === "symbol" ? "NaN" : formatNumber(Number(a));
          break;
        case "i":
          piece = typeof a === "bigint" ? a + "n" : typeof a === "symbol" ? "NaN" : formatNumber(parseInt(a));
          break;
        case "f":
          piece = typeof a === "symbol" ? "NaN" : formatNumber(parseFloat(a));
          break;
        case "j":
          try { piece = JSON.stringify(a); } catch (e) { piece = "[Circular]"; }
          break;
        case "o": piece = inspect(a, { depth: 4 }); break;
        case "O": piece = inspect(a); break;
        case "c": piece = ""; break;
        default: continue;
        }
        out += first.slice(last, at) + piece;
        last = at + 2;
        at++;
        i++;
      }
      out += first.slice(last);
    }
    for (; i < args.length; i++) {
      if (i > 0) out += " ";
      out += typeof args[i] === "string" ? args[i] : inspect(args[i]);
    }
    return out;
  }

  var groupIndent = "", counts = {}, timers = {};
  function emit(fd, text) {
    if (groupIndent !== "") text = groupIndent + text.split("\n").join("\n" + groupIndent);
    host.write(fd, text + "\n");
  }
  function out() { emit(1, formatWithOptions(Array.prototype.slice.call(arguments))); }
  function err() { emit(2, formatWithOptions(Array.prototype.slice.call(arguments))); }
  var console = {
    log: out, info: out, debug: out, warn: err, error: err,
    trace: function () {
      var e = new Error(formatWithOptions(Array.prototype.slice.call(arguments)));
      var stack = String(e.stack).split("\n");
      emit(2, "Trace" + (e.message !== "" ? ": " + e.message : "") + (stack.length > 2 ? "\n" + stack.slice(2).join("\n") : ""));
    },
    assert: function (cond) {
      if (cond) return;
      var rest = Array.prototype.slice.call(arguments, 1);
      emit(2, "Assertion failed" + (rest.length ? ": " + formatWithOptions(rest) : ""));
    },
    dir: function (v, opts) { emit(1, inspect(v, opts)); },
    group: function () { if (arguments.length) out.apply(null, arguments); groupIndent += "  "; },
    groupEnd: function () { groupIndent = groupIndent.slice(0, -2); },
    count: function (label) { label = label === undefined ? "default" : String(label); counts[label] = (counts[label] || 0) + 1; emit(1, label + ": " + counts[label]); },
    countReset: function (label) { counts[label === undefined ? "default" : String(label)] = 0; },
    time: function (label) { timers[label === undefined ? "default" : String(label)] = host.now(); },
    timeEnd: function (label) { console.timeLog(label); delete timers[label === undefined ? "default" : String(label)]; },
    timeLog: function (label) {
      label = label === undefined ? "default" : String(label);
      if (!(label in timers)) { emit(2, "Warning: No such label '" + label + "' for console.timeLog()"); return; }
      var ms = host.now() - timers[label], extra = Array.prototype.slice.call(arguments, 1);
      emit(1, label + ": " + (ms < 1000 ? ms.toFixed(3) + "ms" : (ms / 1000).toFixed(3) + "s") + (extra.length ? " " + formatWithOptions(extra) : ""));
    }
  };
  for (var name in console) Object.defineProperty(console, name, { enumerable: true, writable: true, configurable: true, value: console[name] });
  Object.defineProperty(globalThis, "console", { value: console, writable: true, configurable: true, enumerable: false });
  Object.defineProperty(globalThis, "print", { value: out, writable: true, configurable: true, enumerable: false });
  return { inspect: inspect, format: function () { return formatWithOptions(Array.prototype.slice.call(arguments)); } };
})
);

#endif
