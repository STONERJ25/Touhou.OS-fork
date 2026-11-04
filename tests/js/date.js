// Date (ECMA-262 21.4 and Annex B.2.3).  The host of this test (tests/host/jstool.c) is in UTC; the host test
// DateZones in tests/host/test_js.py runs the zone-dependent checks with other offsets.  The expected numbers were
// computed with an independent days-from-civil routine, not with the engine.
// Self-checking: every check states the expected value; the file prints its failures and a summary.
var total = 0, fails = 0;
function repr(v) {
  if (typeof v === "string") return JSON.stringify(v);
  if (Array.isArray(v)) return "[" + v.map(repr).join(",") + "]";
  if (Object.is(v, -0)) return "-0";
  return String(v);
}
function eq(actual, expected, label) {
  total++;
  var a = repr(actual), e = repr(expected);
  if (a !== e) { fails++; print("FAIL " + label + ": got " + a + ", want " + e); }
}
function throws(f, ctor, label) {
  total++;
  try { f(); } catch (e) { if (e instanceof ctor) return; fails++; print("FAIL " + label + ": wrong error " + e); return; }
  fails++; print("FAIL " + label + ": no error");
}

// ---- Date.UTC and the time value
eq(Date.UTC(1970, 0, 1), 0, "the epoch");
eq(Date.UTC(2000, 0, 1), 946684800000, "2000-01-01");
eq(Date.UTC(2020, 1, 29, 12, 34, 56, 789), 1582979696789, "a leap day with a time");
eq(Date.UTC(1999, 11, 31, 23, 59, 59, 999), 946684799999, "the last millisecond of 1999");
eq(Date.UTC(1, 0, 1), -2177452800000, "years 0..99 mean 1900..1999: year 1 is 1901");
eq(new Date(0).setUTCFullYear(1, 0, 1), -62135596800000, "the real year 1 through setUTCFullYear");
eq(Date.UTC(99, 0, 1), Date.UTC(1999, 0, 1), "99 is 1999");
eq(Date.UTC(100, 0, 1), -59011459200000, "year 100 is year 100");
eq(Date.UTC(-1, 0, 1), -62198755200000, "year -1");
eq(Date.UTC(275760, 8, 13), 8640000000000000, "the latest time");
eq(Date.UTC(275760, 8, 13, 0, 0, 0, 1), NaN, "one millisecond later is out of range");
eq(Date.UTC(-271821, 3, 20), -8640000000000000, "the earliest time");
eq(Date.UTC(2000), 946684800000, "only a year");
eq(Date.UTC(), NaN, "no arguments");
eq(Date.UTC(2000, 12), Date.UTC(2001, 0), "month overflow carries into the year");
eq(Date.UTC(2000, -1), Date.UTC(1999, 11), "negative months borrow");
eq(Date.UTC(2000, 0, 0), Date.UTC(1999, 11, 31), "day 0 is the last day of the previous month");
eq(Date.UTC(2000, 0, 1, 24), Date.UTC(2000, 0, 2), "hour 24");
eq(Date.UTC(2000, 0, 1, 0, 0, 0, 1000), Date.UTC(2000, 0, 1, 0, 0, 1), "milliseconds overflow");
eq(Date.UTC(2000.9, 0.9, 1.9), Date.UTC(2000, 0, 1), "arguments are truncated");
eq(Date.UTC(2000, NaN), NaN, "NaN argument");
eq(Date.UTC(2000, Infinity), NaN, "infinite argument");
eq(Date.UTC(2000, 1, 29) - Date.UTC(2000, 1, 28), 86400000, "2000 is a leap year");
eq(Date.UTC(1900, 1, 29), Date.UTC(1900, 2, 1), "1900 is not");
eq(Date.UTC(2100, 2, 1), 4107542400000, "2100 is not either");
eq(Date.UTC(2000, 1, 29), 951782400000, "a day in a century leap year");

// ---- construction
eq(new Date(0).getTime(), 0, "from a number");
eq(new Date(8.64e15).getTime(), 8.64e15, "the limit");
eq(new Date(8.64e15 + 1).getTime(), NaN, "past the limit");
eq(new Date(-8.64e15).getTime(), -8.64e15, "the other limit");
eq(new Date(1.9).getTime(), 1, "truncated");
eq(new Date(-1.9).getTime(), -1, "truncated toward zero");
eq(new Date(NaN).getTime(), NaN, "NaN");
eq(new Date(new Date(5)).getTime(), 5, "from a Date");
eq(new Date("2000-01-01").getTime(), 946684800000, "from a string");
eq(new Date(true).getTime(), 1, "ToNumber of a boolean");
eq(new Date(null).getTime(), 0, "null is 0");
eq(new Date(undefined).getTime(), NaN, "undefined is NaN");
eq(new Date({ valueOf: function () { return 7; } }).getTime(), 7, "valueOf is used");
eq(new Date({ toString: function () { return "2000-01-01T00:00:00Z"; }, valueOf: null }).getTime(), 946684800000, "an object that converts to a string is parsed");
eq(new Date(2000, 0).getTime(), 946684800000, "year and month (UTC host)");
eq(new Date(2000, 0, 1, 0, 0, 0, 0).getTime(), 946684800000, "all components");
eq(new Date(99, 0).getFullYear(), 1999, "two-digit year");
eq(new Date(2019, 12).getFullYear() + "-" + new Date(2019, 12).getMonth(), "2020-0", "month 12 is January of the next year");
eq(typeof Date(), "string", "called as a function it returns a string");
eq(typeof Date(2000, 1), "string", "arguments are ignored then");
eq(Date.length, 7, "Date.length");
eq(Date.name, "Date", "Date.name");
eq(Date.UTC.length, 7, "Date.UTC.length");
eq(Date.prototype.constructor === Date, true, "prototype.constructor");
eq(Object.prototype.toString.call(new Date(0)), "[object Date]", "class");
eq(Object.prototype.toString.call(Date.prototype), "[object Object]", "Date.prototype is an ordinary object");
throws(function () { Date.prototype.getTime.call({}); }, TypeError, "methods need a Date");
throws(function () { Date.prototype.getTime.call(Date.prototype); }, TypeError, "Date.prototype is not a Date");
eq(Date.now() > 1.7e12, true, "Date.now is after 2023");
eq(typeof Date.now(), "number", "Date.now is a number");
class MyDate extends Date { year() { return this.getUTCFullYear(); } }
eq(new MyDate(0).year(), 1970, "subclassing");
eq(Reflect.construct(Date, [0], Object).getTime === undefined, true, "newTarget decides the prototype");

// ---- getters
var d = new Date(Date.UTC(2020, 1, 29, 12, 34, 56, 789));
eq([d.getUTCFullYear(), d.getUTCMonth(), d.getUTCDate(), d.getUTCDay(), d.getUTCHours(), d.getUTCMinutes(), d.getUTCSeconds(), d.getUTCMilliseconds()],
   [2020, 1, 29, 6, 12, 34, 56, 789], "UTC getters");
eq([d.getFullYear(), d.getMonth(), d.getDate(), d.getDay(), d.getHours(), d.getMinutes(), d.getSeconds(), d.getMilliseconds(), d.getTimezoneOffset()],
   [2020, 1, 29, 6, 12, 34, 56, 789, 0], "local getters in a UTC host");
eq([d.getYear(), d.valueOf(), +d, d.getTime()], [120, 1582979696789, 1582979696789, 1582979696789], "getYear (Annex B) and valueOf");
eq([new Date(0).getDay(), new Date(-1).getDay(), new Date(-1).getUTCFullYear(), new Date(-1).getUTCMilliseconds()], [4, 3, 1969, 999], "before the epoch");
eq([new Date(NaN).getFullYear(), new Date(NaN).getDay(), new Date(NaN).getTimezoneOffset(), new Date(NaN).getYear()], [NaN, NaN, NaN, NaN], "an invalid Date");
eq([new Date(Date.UTC(-1, 0, 1)).getUTCFullYear(), new Date(Date.UTC(0, 0, 1)).getUTCFullYear(), new Date(-62167219200000).getUTCFullYear()], [-1, 1900, 0], "years around zero (the 0 here is the 1900 mapping, the third is year 0)");
var days = [];
for (var i = 0; i < 7; i++) days.push(new Date(Date.UTC(2000, 0, 2 + i)).getUTCDay());
eq(days, [0, 1, 2, 3, 4, 5, 6], "days of the week");
eq([new Date(Date.UTC(2100, 2, 1)).getUTCDay(), new Date(Date.UTC(2024, 1, 29)).getUTCDay()], [1, 4], "weekdays of some dates");

// ---- setters
var s = new Date(Date.UTC(2000, 0, 31, 10, 20, 30, 400));
eq(s.setUTCMonth(1), Date.UTC(2000, 2, 2, 10, 20, 30, 400), "setting February on Jan 31 overflows to March 2");
s = new Date(Date.UTC(2000, 0, 15));
eq([s.setUTCDate(0), s.getUTCMonth(), s.getUTCDate()], [Date.UTC(1999, 11, 31), 11, 31], "setUTCDate(0)");
s = new Date(0);
eq([s.setUTCHours(25), s.getUTCDate(), s.getUTCHours()], [90000000, 2, 1], "setUTCHours overflow");
eq(s.setUTCMinutes(61), 93660000, "setUTCMinutes returns the new time value");
s = new Date(0);
s.setUTCHours(1, 2, 3, 4);
eq([s.getUTCHours(), s.getUTCMinutes(), s.getUTCSeconds(), s.getUTCMilliseconds()], [1, 2, 3, 4], "multiple arguments");
s.setUTCSeconds(59, 999);
eq([s.getUTCSeconds(), s.getUTCMilliseconds()], [59, 999], "setUTCSeconds with ms");
s.setUTCMilliseconds(1001);
eq([s.getUTCMinutes(), s.getUTCSeconds(), s.getUTCMilliseconds()], [3, 0, 1], "ms carries into seconds and minutes (01:02:59.999 + 1.001 s)");
s = new Date(0);
eq([s.setUTCFullYear(2001, 5, 7), s.getUTCFullYear(), s.getUTCMonth(), s.getUTCDate()], [Date.UTC(2001, 5, 7), 2001, 5, 7], "setUTCFullYear with month and day");
eq(s.setTime(5), 5, "setTime");
eq(s.setTime(NaN), NaN, "setTime(NaN) makes it invalid");
eq(s.getTime(), NaN, "and it stays so");
eq(s.setUTCHours(1), NaN, "setting a field of an invalid date does nothing");
eq(s.setUTCFullYear(2000), Date.UTC(2000, 0, 1), "except the year: an invalid date is treated as +0");
s = new Date(0);
eq(s.setUTCMonth(NaN), NaN, "setting NaN invalidates");
eq(new Date(0).setMilliseconds(), NaN, "no argument is undefined: NaN");
eq(new Date(5).setTime(8.64e15 + 1), NaN, "setTime clips");
eq([new Date(0).setFullYear(1999), new Date(0).setMonth(11), new Date(0).setDate(2), new Date(0).setHours(1), new Date(0).setMinutes(1), new Date(0).setSeconds(1), new Date(0).setMilliseconds(1)],
   [Date.UTC(1999, 0, 1), Date.UTC(1970, 11, 1), 86400000, 3600000, 60000, 1000, 1], "local setters in a UTC host");
var y = new Date(0);
y.setYear(99);
eq([y.getFullYear(), new Date(0).setYear(2005) === Date.UTC(2005, 0, 1), new Date(0).setYear(NaN)], [1999, true, NaN], "setYear (Annex B)");
eq(Date.prototype.toGMTString === Date.prototype.toUTCString, true, "toGMTString is toUTCString");
eq([Date.prototype.setHours.length, Date.prototype.setUTCFullYear.length, Date.prototype.setMonth.length, Date.prototype.setMilliseconds.length, Date.prototype.setSeconds.length], [4, 3, 2, 1, 2], "setter lengths");

// ---- formatting
var f = new Date(Date.UTC(2020, 1, 29, 12, 34, 56, 789));
eq(f.toISOString(), "2020-02-29T12:34:56.789Z", "toISOString");
eq(new Date(0).toISOString(), "1970-01-01T00:00:00.000Z", "toISOString of the epoch");
eq(new Date(Date.UTC(-1, 0, 1)).toISOString(), "-000001-01-01T00:00:00.000Z", "negative years have six digits");
eq(new Date(-62167219200000).toISOString(), "0000-01-01T00:00:00.000Z", "year 0 has four");
eq(new Date(8.64e15).toISOString(), "+275760-09-13T00:00:00.000Z", "big years have a plus sign");
eq(new Date(-62167219200001).toISOString(), "-000001-12-31T23:59:59.999Z", "the millisecond before year 0");
throws(function () { new Date(NaN).toISOString(); }, RangeError, "toISOString of an invalid date");
eq(f.toUTCString(), "Sat, 29 Feb 2020 12:34:56 GMT", "toUTCString");
eq(new Date(Date.UTC(-1, 0, 1)).toUTCString(), "Fri, 01 Jan -0001 00:00:00 GMT", "toUTCString with a negative year");
eq(f.toDateString(), "Sat Feb 29 2020", "toDateString");
eq(f.toTimeString().slice(0, 17), "12:34:56 GMT+0000", "toTimeString starts with the time and the offset");
eq(f.toString().slice(0, 33), "Sat Feb 29 2020 12:34:56 GMT+0000", "toString");
eq(new Date(NaN).toString(), "Invalid Date", "toString of an invalid date");
eq([new Date(NaN).toDateString(), new Date(NaN).toTimeString(), new Date(NaN).toUTCString(), String(new Date(NaN))], ["Invalid Date", "Invalid Date", "Invalid Date", "Invalid Date"], "all invalid");
eq(typeof f.toLocaleString() + typeof f.toLocaleDateString() + typeof f.toLocaleTimeString(), "stringstringstring", "locale strings exist");
eq(new Date(NaN).toLocaleString(), "Invalid Date", "toLocaleString of an invalid date");
eq(f.toJSON(), "2020-02-29T12:34:56.789Z", "toJSON");
eq(new Date(NaN).toJSON(), null, "toJSON of an invalid date is null");
eq(JSON.stringify({ when: new Date(0) }), '{"when":"1970-01-01T00:00:00.000Z"}', "JSON.stringify");
eq(Date.prototype.toJSON.call({ toISOString: function () { return "x"; } }), "x", "toJSON is generic");
eq(new Date(Date.UTC(2005, 5, 7, 8, 9, 10)).toString().slice(0, 24), "Tue Jun 07 2005 08:09:10", "zero padding");
eq(new Date(Date.UTC(12, 0, 1)).getUTCFullYear(), 1912, "two-digit year in UTC");
eq(new Date(Date.UTC(1, 0, 1)).toString().slice(0, 15), "Tue Jan 01 1901", "year 1 maps to 1901");
eq(new Date(-62135596800000).toString().slice(0, 15), "Mon Jan 01 0001", "the year 1 is padded to four digits");

// ---- conversion to primitives
eq(typeof (new Date(0) + 1), "string", "+ uses the string hint");
eq(new Date(5) - 1, 4, "- uses the number hint");
eq(new Date(5) * 2, 10, "* uses the number hint");
eq(new Date(5) < new Date(6), true, "comparison");
eq(new Date(5) == 5, false, "== on a Date and a number compares the string");
eq(new Date(5) - new Date(2), 3, "difference");
eq(`${new Date(0)}`.slice(0, 15), "Thu Jan 01 1970", "template literal");
eq(new Date(0)[Symbol.toPrimitive]("number"), 0, "toPrimitive number");
eq(typeof new Date(0)[Symbol.toPrimitive]("default"), "string", "toPrimitive default is the string");
eq(new Date(0)[Symbol.toPrimitive]("string").slice(0, 3), "Thu", "toPrimitive string");
throws(function () { new Date(0)[Symbol.toPrimitive]("bogus"); }, TypeError, "toPrimitive with a bad hint");
throws(function () { Date.prototype[Symbol.toPrimitive].call(1, "number"); }, TypeError, "toPrimitive on a non-object");
eq(Date.prototype[Symbol.toPrimitive].name, "[Symbol.toPrimitive]", "its name");
eq(Object.getOwnPropertyDescriptor(Date.prototype, Symbol.toPrimitive).writable, false, "is not writable");
var cmp = [new Date(3), new Date(1), new Date(2)].sort(function (a, b) { return a - b; });
eq(cmp.map(Number), [1, 2, 3], "sort by difference");
eq(Math.max(new Date(1), new Date(9)), 9, "Math.max converts");
eq(isNaN(new Date("garbage")), true, "isNaN of an invalid date");

// ---- parsing
eq(Date.parse("2000-01-01"), 946684800000, "date only is UTC");
eq(Date.parse("2000-01"), 946684800000, "year and month");
eq(Date.parse("2000"), 946684800000, "year only");
eq(Date.parse("2000-01-01T00:00:00Z"), 946684800000, "with Z");
eq(Date.parse("2000-01-01T00:00Z"), 946684800000, "without seconds");
eq(Date.parse("2000-01-01T00:00:00.5Z"), 946684800500, "one fractional digit");
eq(Date.parse("2000-01-01T00:00:00.123456Z"), 946684800123, "extra fraction digits are truncated");
eq(Date.parse("2016-06-15T10:20:30+05:30"), 1465966230000, "with an offset");
eq(Date.parse("2016-06-15T10:20:30-00:30"), 1465986030000 + 30 * 60000, "with a negative offset");
eq(Date.parse("2000-01-01T00:00:00"), 946684800000, "no offset means local time (UTC here)");
eq(Date.parse("2000-01-01T24:00:00Z"), 946771200000, "24:00 is the end of the day");
eq(Date.parse("2000-01-01T24:00:01Z"), NaN, "but nothing after it");
eq(Date.parse("2000-13-01"), NaN, "month 13");
eq(Date.parse("2000-01-01T25:00Z"), NaN, "hour 25");
eq(Date.parse("2000-01-01T00:60Z"), NaN, "minute 60");
eq(Date.parse("+275760-09-13T00:00:00.000Z"), 8.64e15, "extended year, the latest time");
eq(Date.parse("+275760-09-13T00:00:00.001Z"), NaN, "one millisecond later");
eq(Date.parse("-271821-04-20T00:00:00.000Z"), -8.64e15, "extended negative year, the earliest time");
eq(Date.parse("-000001-01-01T00:00:00Z"), -62198755200000, "year -1");
eq(Date.parse("-000000-01-01T00:00:00Z"), NaN, "minus zero is not a valid year");
eq(Date.parse("+002000-01-01T00:00:00Z"), 946684800000, "six-digit year");
eq(Date.parse("0000-01-01T00:00:00Z"), -62167219200000, "year 0");
eq(Date.parse(""), NaN, "empty string");
eq(Date.parse("not a date"), NaN, "garbage");
eq(Date.parse(new Date(0).toString()), 0, "toString output round-trips");
eq(Date.parse(new Date(0).toUTCString()), 0, "toUTCString output round-trips");
eq(Date.parse(new Date(1234567000).toISOString()), 1234567000, "toISOString output round-trips");
eq(Date.parse("Thu, 01 Jan 1970 00:00:00 GMT"), 0, "the UTC format");
eq(Date.parse("Thu Jan 01 1970 00:00:00 GMT+0100"), -3600000, "the toString format with an offset");
eq(Date.parse("Thu Jan 01 1970 00:00:00 GMT+0100 (CET)"), -3600000, "with a name in parentheses");
// the formats below are the engine's legacy parser, which follows what browsers accept; the specification leaves them open
eq(Date.parse("Jan 1 2000"), 946684800000, "a legacy format");
eq(Date.parse("1 Jan 2000 00:00:00 UTC"), 946684800000, "another one");
eq(Date.parse("January 1, 2000"), 946684800000, "with a month name");
eq(Date.parse("Jan 1 2000 10:00 PM"), 946684800000 + 22 * 3600000, "AM/PM");
eq(Date.parse("1/2/2000"), Date.UTC(2000, 0, 2), "month/day/year");
eq(Date.parse("Sat, 01 Jan 2000 00:00:00 GMT"), 946684800000, "RFC 2822");
eq(Date.parse("Sat, 01 Jan 2000 00:00:00 +0200"), 946684800000 - 7200000, "RFC 2822 with an offset");
eq(Date.parse(String(new Date(Date.UTC(2020, 1, 29, 12, 34, 56)))), Date.UTC(2020, 1, 29, 12, 34, 56), "round trip of a toString");
eq(Date.parse.length, 1, "Date.parse.length");
eq(new Date("2000-01-01T00:00:00Z").getTime(), new Date(Date.parse("2000-01-01T00:00:00Z")).getTime(), "the constructor parses strings the same way");

// ---- Date.prototype property list
var names = Object.getOwnPropertyNames(Date.prototype).sort().join();
eq(names, "constructor,getDate,getDay,getFullYear,getHours,getMilliseconds,getMinutes,getMonth,getSeconds,getTime,getTimezoneOffset,getUTCDate,getUTCDay,getUTCFullYear,getUTCHours,getUTCMilliseconds,getUTCMinutes,getUTCMonth,getUTCSeconds,getYear,setDate,setFullYear,setHours,setMilliseconds,setMinutes,setMonth,setSeconds,setTime,setUTCDate,setUTCFullYear,setUTCHours,setUTCMilliseconds,setUTCMinutes,setUTCMonth,setUTCSeconds,setYear,toDateString,toGMTString,toISOString,toJSON,toLocaleDateString,toLocaleString,toLocaleTimeString,toString,toTimeString,toUTCString,valueOf", "Date.prototype has these properties");

print("date: " + total + " checks, " + fails + " failures");
