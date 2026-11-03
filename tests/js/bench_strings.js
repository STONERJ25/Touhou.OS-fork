// benchmark: string building, concatenation and joins
var parts = [];
for (var i = 0; i < 200000; i++) parts.push("item" + i);
var s = parts.join(",");
var t = "";
for (var j = 0; j < 100000; j++) t += String.fromCharCode(97 + j % 26);
print("strings", s.length, t.length, t.slice(-3), s.indexOf("item199999"));
