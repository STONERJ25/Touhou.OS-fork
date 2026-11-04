// regular expressions on a megabyte of text: global match, replace, split, exec loops, alternation, backtracking
var words = ["alpha", "beta", "gamma", "delta", "epsilon", "zeta", "eta", "theta"];
var parts = [];
for (var i = 0; i < 40000; i++) parts.push(words[i % 8] + i + (i % 7 == 0 ? "@example.com\n" : " "));
var text = parts.join("");
var numbers = text.match(/\d+/g).length;
var emails = text.match(/[a-z]+\d*@[a-z]+\.com/g).length;
var squeezed = text.replace(/\s+/g, "_").length;
var pieces = text.split(/[ @.\n]/).length;
var sum = 0, re = /([a-z]+?)(\d+)/g, mm;
while ((mm = re.exec(text))) sum += mm[2].length;
var alt = text.match(/\b(?:alpha|gamma|theta)\d+\b/g).length;
var lines = text.match(/^.*example.*$/gm).length;
var ci = text.match(/EPSILON\d+/gi).length;
var fn = text.replace(/\d+/g, function (d) { return d.length; }).length;
var back = 0;
for (var j = 0; j < 20000; j++) if (/^(\w+)-\1$/.test("word" + j + "-word" + j)) back++;
print("regexp", numbers, emails, squeezed, pieces, sum, alt, lines, ci, fn, back);
