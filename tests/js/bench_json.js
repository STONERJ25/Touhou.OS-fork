// benchmark: JSON stringify and parse round trips
var data = [];
for (var i = 0; i < 20000; i++) data.push({ id: i, name: "n" + i, tags: ["a", "b", i % 7], ok: i % 2 === 0, v: i / 4 });
var total = 0;
for (var k = 0; k < 5; k++) { var text = JSON.stringify(data); var back = JSON.parse(text); total += back.length + text.length; }
print("json", total);
