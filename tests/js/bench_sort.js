// benchmark: sorting numbers with a comparator and strings with the default order
var seed = 12345;
function rnd() { seed = (seed * 1103515245 + 12345) & 0x7fffffff; return seed; }
var nums = []; for (var i = 0; i < 200000; i++) nums.push(rnd() % 1000000);
nums.sort((a, b) => a - b);
var ok = true; for (var j = 1; j < nums.length; j++) if (nums[j - 1] > nums[j]) ok = false;
var strs = []; for (var k = 0; k < 50000; k++) strs.push("s" + (rnd() % 100000));
strs.sort();
print("sort", ok, nums[0] <= nums[nums.length - 1], strs[0] <= strs[strs.length - 1], nums.length + strs.length);
