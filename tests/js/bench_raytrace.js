// benchmark: a tiny ray tracer (objects, method calls, floating point)
class V {
    constructor(x, y, z) { this.x = x; this.y = y; this.z = z; }
    add(o) { return new V(this.x + o.x, this.y + o.y, this.z + o.z); }
    sub(o) { return new V(this.x - o.x, this.y - o.y, this.z - o.z); }
    mul(k) { return new V(this.x * k, this.y * k, this.z * k); }
    dot(o) { return this.x * o.x + this.y * o.y + this.z * o.z; }
    norm() { return this.mul(1 / Math.sqrt(this.dot(this))); }
}
var spheres = [{ c: new V(0, -1, 3), r: 1, col: 1 }, { c: new V(2, 0, 4), r: 1, col: 2 },
               { c: new V(-2, 0, 4), r: 1, col: 3 }, { c: new V(0, -5001, 0), r: 5000, col: 4 }];
var light = new V(2, 1, 0);
function hit(o, d) {
    var best = Infinity, bs = null;
    for (var s of spheres) {
        var co = o.sub(s.c), b = 2 * co.dot(d), c = co.dot(co) - s.r * s.r, disc = b * b - 4 * c;
        if (disc < 0) continue;
        var t = (-b - Math.sqrt(disc)) / 2;
        if (t > 0.001 && t < best) { best = t; bs = s; }
    }
    return bs ? { t: best, s: bs } : null;
}
var W = 160, H = 120, sum = 0;
for (var y = 0; y < H; y++)
    for (var x = 0; x < W; x++) {
        var d = new V((x - W / 2) / W, -(y - H / 2) / W, 1).norm(), h = hit(new V(0, 0, 0), d);
        if (h) {
            var p = d.mul(h.t), n = p.sub(h.s.c).norm(), l = light.sub(p).norm();
            sum += h.s.col * Math.max(0, n.dot(l));
        }
    }
print("raytrace", Math.round(sum));
