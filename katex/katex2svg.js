// katex2svg.js
//
// Converts LaTeX math to a standalone SVG by running KaTeX (renderToString,
// HTML output) and flattening the resulting span tree into SVG primitives,
// following the layout rules of KaTeX's own stylesheet (katex.css).
//
// KaTeX positions everything with em-based inline styles (vlist `top`
// offsets, `mspace` margins, sizing classes), so the layout is
// deterministic and does not require a DOM:
//
//   * inline flow: children are laid out left-to-right on a shared baseline
//   * .vlist-t: an inline table of height 0 at the baseline; its items are
//     positioned by their `top: Xem` inline style (negative = above the
//     baseline) and aligned (default: centre) on the vlist's content width
//   * .mspace / .nulldelimiter / .arraycolsep: fixed-width spacers
//   * .sizing.reset-sizeA.sizeB / .mtight: font-size scaling (em lengths in
//     the inline styles of the subtree are interpreted in the scaled context)
//   * .frac-line / .overline-line / .underline-line: CSS borders, drawn as
//     horizontal rules spanning the vlist width
//   * .hide-tail: KaTeX's own inline <svg> (radical tail + overbar),
//     flattened to a polygon, cropped to its preserveAspectRatio="slice"
//     box, and emitted as a plain path (the host SVG renderer mishandles
//     clip-path, so the cropping is done up front)
//   * .mtable: matrix/array columns, each a stack of vlists on a shared
//     baseline, separated by .arraycolsep spacers
//
// Glyphs are drawn as SVG <text> with KaTeX's own font faces, which the
// host registers as application fonts from bundled resources (the family
// names match katex.css's @font-face rules); the text measurement is
// bridged to the host through globalThis.__qt.measureText(text, family,
// sizePx, weight, fontStyle) -> { width, ascent, descent } (provided by the
// QuickJS engine; the Node test harness supplies a stub).
//
// globalThis.__katexToSvg(tex, { displayMode, fontSize, color })
//   -> { svg, width, height, baseline }  (px), or null on a LaTeX error.

(function (g) {
  'use strict';

  // KaTeX font-size steps (katex.css .sizing.reset-size1.sizeN).
  var SIZE = { 1: 1, 2: 1.2, 3: 1.4, 4: 1.6, 5: 1.8, 6: 2, 7: 2.4, 8: 2.88, 9: 3.456, 10: 4.1472 };

  // Glyph class -> KaTeX font face, following the font-family rules of
  // KaTeX's own stylesheet (katex.css). The faces are registered as
  // application fonts by the host; unknown classes fall back to KaTeX_Main,
  // the stylesheet's base font.
  var FONTS = {
    mathnormal: { family: 'KaTeX_Math', style: 'italic', weight: 400 },
    mathit: { family: 'KaTeX_Main', style: 'italic', weight: 400 },
    mathbf: { family: 'KaTeX_Main', style: 'normal', weight: 700 },
    boldsymbol: { family: 'KaTeX_Math', style: 'italic', weight: 700 },
    amsrm: { family: 'KaTeX_AMS', style: 'normal', weight: 400 },
    mathbb: { family: 'KaTeX_AMS', style: 'normal', weight: 400 },
    textbb: { family: 'KaTeX_AMS', style: 'normal', weight: 400 },
    mathcal: { family: 'KaTeX_Caligraphic', style: 'normal', weight: 400 },
    mathfrak: { family: 'KaTeX_Fraktur', style: 'normal', weight: 400 },
    textfrak: { family: 'KaTeX_Fraktur', style: 'normal', weight: 400 },
    mathboldfrak: { family: 'KaTeX_Fraktur', style: 'normal', weight: 700 },
    textboldfrak: { family: 'KaTeX_Fraktur', style: 'normal', weight: 700 },
    mathtt: { family: 'KaTeX_Typewriter', style: 'normal', weight: 400 },
    mathscr: { family: 'KaTeX_Script', style: 'normal', weight: 400 },
    textscr: { family: 'KaTeX_Script', style: 'normal', weight: 400 },
    mathsf: { family: 'KaTeX_SansSerif', style: 'normal', weight: 400 },
    textsf: { family: 'KaTeX_SansSerif', style: 'normal', weight: 400 },
    mathboldsf: { family: 'KaTeX_SansSerif', style: 'normal', weight: 700 },
    textboldsf: { family: 'KaTeX_SansSerif', style: 'normal', weight: 700 },
    mathsfit: { family: 'KaTeX_SansSerif', style: 'italic', weight: 400 },
    mathitsf: { family: 'KaTeX_SansSerif', style: 'italic', weight: 400 },
    textitsf: { family: 'KaTeX_SansSerif', style: 'italic', weight: 400 },
    textrm: { family: 'KaTeX_Main', style: 'normal', weight: 400 },
    mainrm: { family: 'KaTeX_Main', style: 'normal', weight: 400 },
    mathrm: { family: 'KaTeX_Main', style: 'normal', weight: 400 },
    textbf: { family: 'KaTeX_Main', style: 'normal', weight: 700 },
    textit: { family: 'KaTeX_Main', style: 'italic', weight: 400 },
    texttt: { family: 'KaTeX_Typewriter', style: 'normal', weight: 400 },
    text: { family: 'KaTeX_Main', style: 'normal', weight: 400 },
    mathdefault: { family: 'KaTeX_Main', style: 'normal', weight: 400 },
    delimsizing: { family: 'KaTeX_Main', style: 'normal', weight: 400 }
  };

  // The dedicated faces for stretchy delimiters and large operators
  // (katex.css .delimsizing.sizeN, .delim-sizeN, .op-symbol.*).
  var SIZE_FONTS = {
    size1: 'KaTeX_Size1', size2: 'KaTeX_Size2',
    size3: 'KaTeX_Size3', size4: 'KaTeX_Size4'
  };

  function decode(s) {
    return s.replace(/&(#x[0-9a-fA-F]+|#[0-9]+|[a-zA-Z]+);/g, function (_, e) {
      if (e[0] === '#') {
        var c = (e[1] === 'x' || e[1] === 'X') ? parseInt(e.slice(2), 16) : parseInt(e.slice(1), 10);
        return c ? String.fromCodePoint(c) : _;
      }
      var map = { amp: '&', lt: '<', gt: '>', quot: '"', apos: "'", nbsp: '\u00a0' };
      return Object.prototype.hasOwnProperty.call(map, e) ? map[e] : _;
    });
  }

  // Minimal HTML parser: elements (name, attrs, children) and text nodes.
  function parseHtml(html) {
    var root = { type: 'el', name: '#root', attrs: {}, children: [] };
    var stack = [root];
    var i = 0;
    var n = html.length;
    function pushText(s) {
      s = decode(s);
      if (s)
        stack[stack.length - 1].children.push({ type: 'text', data: s });
    }
    var attrRe = /([a-zA-Z_:.-]+)\s*=\s*"([^"]*)"|([a-zA-Z_:.-]+)\s*=\s*'([^']*)'|([a-zA-Z_:.-]+)/g;
    while (i < n) {
      var lt = html.indexOf('<', i);
      if (lt < 0) {
        pushText(html.slice(i));
        break;
      }
      if (lt > i)
        pushText(html.slice(i, lt));
      var gt = html.indexOf('>', lt);
      if (gt < 0) {
        pushText(html.slice(lt));
        break;
      }
      var tag = html.slice(lt + 1, gt);
      if (tag[0] === '/') {
        if (stack.length > 1)
          stack.pop();
      } else if (tag[0] !== '!') {
        var selfClose = tag.charAt(tag.length - 1) === '/';
        if (selfClose)
          tag = tag.slice(0, -1);
        var nm = tag.match(/^[a-zA-Z0-9-]+/);
        var name = nm ? nm[0].toLowerCase() : '?';
        var attrs = {};
        var rest = tag.replace(/^[a-zA-Z0-9-]+\s*/, '');
        attrRe.lastIndex = 0;
        var m;
        while ((m = attrRe.exec(rest))) {
          if (m[1] !== undefined)
            attrs[m[1].toLowerCase()] = m[2];
          else if (m[3] !== undefined)
            attrs[m[3].toLowerCase()] = m[4];
          else
            attrs[m[5].toLowerCase()] = '';
        }
        var el = { type: 'el', name: name, attrs: attrs, children: [] };
        stack[stack.length - 1].children.push(el);
        if (!selfClose && name !== 'path')
          stack.push(el);
      }
      i = gt + 1;
    }
    return root;
  }

  function parseStyle(style) {
    var out = {};
    if (!style)
      return out;
    var parts = style.split(';');
    for (var i = 0; i < parts.length; ++i) {
      var eq = parts[i].indexOf(':');
      if (eq < 0)
        continue;
      out[parts[i].slice(0, eq).trim().toLowerCase()] = parts[i].slice(eq + 1).trim();
    }
    return out;
  }

  // Parse a CSS length as em; a px value is converted with \a unit (px per
  // root em). NaN when the value is missing.
  function emLen(style, key, unit) {
    var v = style[key];
    if (v === undefined)
      return NaN;
    var f = parseFloat(v);
    if (isNaN(f))
      return NaN;
    return v.indexOf('px') >= 0 ? f / unit : f;
  }

  function esc(s) {
    return s.replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;');
  }

  function fmt(v) {
    return String(Math.round(v * 100) / 100);
  }

  // ------------------------------------------------------------------
  // Layout
  //
  // All lengths are in "root em" (1 root em == unit px, unit = 1.21 *
  // fontSize). y grows downward; each element is anchored at its baseline.
  // Layout returns the box { w, top, depth } (distances from the element's
  // baseline) and appends primitives to st.out.
  // ------------------------------------------------------------------

  function katexToSvg(tex, opts) {
    opts = opts || {};
    var basePx = opts.fontSize || 16;
    var unit = 1.21 * basePx; // px per root em
    var color = opts.color || '#000000';

    var html;
    try {
      html = g.katex.renderToString(tex, {
        displayMode: !!opts.displayMode,
        throwOnError: false,
        output: 'html'
      });
    } catch (e) {
      return null;
    }
    if (html.indexOf('katex-error') >= 0)
      return null;

    var tree = parseHtml(html);

    var st = {
      out: [],   // { t: 'text'|'rect'|'poly', ... }
      unit: unit
    };

    function findKatexHtml(node) {
      if (node.type !== 'el')
        return null;
      if (((node.attrs['class'] || '') + ' ').indexOf('katex-html ') >= 0)
        return node;
      for (var i = 0; i < node.children.length; ++i) {
        var r = findKatexHtml(node.children[i]);
        if (r)
          return r;
      }
      return null;
    }
    var htmlRoot = findKatexHtml(tree);
    if (!htmlRoot)
      return null;

    function measure(text, font, scale) {
      var m = g.__qt.measureText(text, font.family, scale * unit, font.weight, font.style);
      return { w: m.width / unit, top: m.ascent / unit, depth: m.descent / unit };
    }

    function textOf(e) {
      var s = '';
      for (var i = 0; i < e.children.length; ++i)
        if (e.children[i].type === 'text')
          s += e.children[i].data;
      return s;
    }

    function fontOf(e) {
      var clsStr = (e.attrs && e.attrs['class']) || '';
      var m = clsStr.match(/delimsizing size([1-4])|delim-size([1-4])/);
      if (m)
        return { family: SIZE_FONTS['size' + (m[1] || m[2])], style: 'normal', weight: 400 };
      if (clsStr.indexOf('op-symbol small-op') >= 0)
        return { family: SIZE_FONTS.size1, style: 'normal', weight: 400 };
      if (clsStr.indexOf('op-symbol large-op') >= 0)
        return { family: SIZE_FONTS.size2, style: 'normal', weight: 400 };
      var cls = clsStr.split(/\s+/);
      for (var i = 0; i < cls.length; ++i)
        if (FONTS[cls[i]])
          return FONTS[cls[i]];
      return FONTS.mathdefault;
    }

    function sizingFactor(cls) {
      var f = 1;
      var r = cls.match(/reset-size(\d+)/);
      var s = cls.match(/\bsize(\d+)/);
      if (r && s && SIZE[+r[1]] && SIZE[+s[1]])
        f *= SIZE[+s[1]] / SIZE[+r[1]];
      if (cls.indexOf('mtight') >= 0)
        f *= 0.7;
      return f;
    }

    // The raw <svg> (KaTeX radical tails) inside \a e, if any.
    function svgOf(e) {
      for (var i = 0; i < e.children.length; ++i)
        if (e.children[i].type === 'el' && e.children[i].name === 'svg')
          return e.children[i];
      return null;
    }

    function pathOf(svgEl) {
      for (var i = 0; i < svgEl.children.length; ++i)
        if (svgEl.children[i].type === 'el' && svgEl.children[i].name === 'path')
          return svgEl.children[i];
      return null;
    }

    // Draw a raw KaTeX <svg> (viewBox units, width/height in em) into the
    // box (bx, by, bw, bh) with the preserveAspectRatio="slice" scaling its
    // attributes declare. The host's SVG renderer (QSvgRenderer) mishandles
    // clip-path (it fills the clip rect), so the path is flattened to
    // polygons, cropped to the box here, and emitted in final coordinates.
    function drawSvg(svgEl, scale, bx, by, bw, bh) {
      var pathEl = pathOf(svgEl);
      if (!pathEl || bw <= 0 || bh <= 0)
        return;
      var vb = (svgEl.attrs['viewbox'] || '0 0 1 1').split(/[\s,]+/).map(parseFloat);
      // "slice": scale so the viewBox covers the box, then crop. k is the
      // em-per-path-unit scale (the larger of the two axis ratios).
      var k = Math.max(bw / vb[2], bh / vb[3]);  // em per path unit
      var subpaths = flattenPath(pathEl.attrs['d'] || '');
      for (var i = 0; i < subpaths.length; ++i) {
        var clipped = clipPolyToRect(subpaths[i], vb[0], vb[1],
                                     vb[0] + bw / k, vb[1] + bh / k);
        if (clipped.length < 3)
          continue;
        var pts = [];
        for (var j = 0; j < clipped.length; ++j)
          pts.push([bx + (clipped[j][0] - vb[0]) * k, by + (clipped[j][1] - vb[1]) * k]);
        st.out.push({ t: 'poly', pts: pts });
      }
    }

    // Parse a path data string into a list of subpaths of [x, y] points,
    // flattening cubic/quadratic beziers and elliptic arcs.
    function flattenPath(d) {
      var toks = d.match(/[a-df-z]|[-+]?(?:[0-9]+\.{0,1}[0-9]*|[0-9]*\.[0-9]+)(?:[eE][-+]?[0-9]+)?/gi) || [];
      var subpaths = [];
      var cur = null;
      var x = 0, y = 0, sx = 0, sy = 0;
      var prevC2 = null, prevQ2 = null;  // 2nd controls for S/T smoothing
      var i = 0;
      function startSub(px, py) {
        cur = [];
        subpaths.push(cur);
        sx = px; sy = py;
        cur.push([px, py]);
        x = px; y = py;
        prevC2 = prevQ2 = null;
      }
      function lineTo(px, py) {
        if (!cur)
          startSub(px, py);
        cur.push([px, py]);
        x = px; y = py;
        prevC2 = prevQ2 = null;
      }
      function cubic(c1x, c1y, c2x, c2y, px, py) {
        if (!cur)
          startSub(x, y);
        var n = 8;
        for (var s = 1; s <= n; ++s) {
          var t = s / n, u = 1 - t;
          cur.push([u*u*u*x + 3*u*u*t*c1x + 3*u*t*t*c2x + t*t*t*px,
                    u*u*u*y + 3*u*u*t*c1y + 3*u*t*t*c2y + t*t*t*py]);
        }
        x = px; y = py;
        prevC2 = [c2x, c2y]; prevQ2 = null;
      }
      function quad(q1x, q1y, px, py) {
        if (!cur)
          startSub(x, y);
        var n = 8;
        for (var s = 1; s <= n; ++s) {
          var t = s / n, u = 1 - t;
          cur.push([u*u*x + 2*u*t*q1x + t*t*px, u*u*y + 2*u*t*q1y + t*t*py]);
        }
        x = px; y = py;
        prevQ2 = [q1x, q1y]; prevC2 = null;
      }
      function arc(rx, ry, rot, large, sweep, px, py) {
        if (rx <= 0 || ry <= 0) {
          lineTo(px, py);
          return;
        }
        rx = Math.abs(rx); ry = Math.abs(ry);
        var cosr = Math.cos(rot), sinr = Math.sin(rot);
        var x1 = cosr * (x - px) / 2 + sinr * (y - py) / 2;
        var y1 = -sinr * (x - px) / 2 + cosr * (y - py) / 2;
        var lambda = x1*x1 / (rx*rx) + y1*y1 / (ry*ry);
        if (lambda > 1) {
          var f = Math.sqrt(lambda);
          rx *= f; ry *= f;
        }
        var sgn = (large === sweep) ? -1 : 1;
        var c = sgn * Math.sqrt(Math.max(0,
            (rx*rx*ry*ry - rx*rx*y1*y1 - ry*ry*x1*x1) /
            (rx*rx*y1*y1 + ry*ry*x1*x1)));
        var cx1 = c * rx * y1 / ry;
        var cy1 = c * ry * x1 / rx;
        var mx = cosr * cx1 - sinr * cy1 + (x + px) / 2;
        var my = sinr * cx1 + cosr * cy1 + (y + py) / 2;
        function pt(a) {
          var ca = Math.cos(a), sa = Math.sin(a);
          return [mx + rx * ca * cosr - ry * sa * sinr,
                  my + rx * ca * sinr + ry * sa * cosr];
        }
        function dpt(a) {
          var ca = Math.cos(a), sa = Math.sin(a);
          return [-rx * sa * cosr - ry * ca * sinr,
                  -rx * sa * sinr + ry * ca * cosr];
        }
        function ang(ux, uy, vx, vy) {
          var dot = ux*vx + uy*vy;
          var len = Math.sqrt((ux*ux + uy*uy) * (vx*vx + vy*vy)) || 1;
          var a = Math.acos(Math.max(-1, Math.min(1, dot / len)));
          if (ux*vy - uy*vx < 0)
            a = -a;
          return a;
        }
        var th1 = ang(1, 0, (x1 - cx1) / rx, (y1 - cy1) / ry);
        var dth = ang((x1 - cx1) / rx, (y1 - cy1) / ry, (-x1 - cx1) / rx, (-y1 - cy1) / ry);
        if (!sweep && dth > 0)
          dth -= 2 * Math.PI;
        else if (sweep && dth < 0)
          dth += 2 * Math.PI;
        if (!cur)
          startSub(x, y);
        var segs = Math.max(1, Math.ceil(Math.abs(dth) / (Math.PI / 4)));
        for (var s = 0; s < segs; ++s) {
          var a1 = th1 + dth * s / segs;
          var a2 = th1 + dth * (s + 1) / segs;
          var alpha = 4 / 3 * Math.tan((a2 - a1) / 4);
          var p1 = pt(a1), p2 = pt(a2), t1 = dpt(a1), t2 = dpt(a2);
          var c1x = p1[0] + alpha * t1[0], c1y = p1[1] + alpha * t1[1];
          var c2x = p2[0] - alpha * t2[0], c2y = p2[1] - alpha * t2[1];
          var n = 4;
          for (var q = 1; q <= n; ++q) {
            var t = q / n, u = 1 - t;
            cur.push([u*u*u*p1[0] + 3*u*u*t*c1x + 3*u*t*t*c2x + t*t*t*p2[0],
                      u*u*u*p1[1] + 3*u*u*t*c1y + 3*u*t*t*c2y + t*t*t*p2[1]]);
          }
        }
        x = px; y = py;
        prevC2 = prevQ2 = null;
      }
      var cmd = null;
      function num() { var v = parseFloat(toks[i++]); return rel ? x + v : v; }
      function numy() { var v = parseFloat(toks[i++]); return rel ? y + v : v; }
      var rel = false;
      while (i < toks.length) {
        if (/[a-df-z]/i.test(toks[i])) {
          cmd = toks[i++];
          rel = cmd === cmd.toLowerCase();
        }
        switch (cmd) {
          case 'm': case 'M':
            startSub(num(), numy());
            // implicit repeats are linetos
            cmd = rel ? 'l' : 'L';
            break;
          case 'l': case 'L':
            lineTo(num(), numy());
            break;
          case 'h': case 'H':
            lineTo(num(), y);
            break;
          case 'v': case 'V':
            lineTo(x, numy());
            break;
          case 'c': case 'C':
            cubic(num(), numy(), num(), numy(), num(), numy());
            break;
          case 's': case 'S':
            cubic(prevC2 ? 2*x - prevC2[0] : x, prevC2 ? 2*y - prevC2[1] : y,
                  num(), numy(), num(), numy());
            break;
          case 'q': case 'Q':
            quad(num(), numy(), num(), numy());
            break;
          case 't': case 'T':
            quad(prevQ2 ? 2*x - prevQ2[0] : x, prevQ2 ? 2*y - prevQ2[1] : y,
                 num(), numy());
            break;
          case 'a': case 'A':
            arc(parseFloat(toks[i++]), parseFloat(toks[i++]),
                parseFloat(toks[i++]) * Math.PI / 180,
                parseInt(toks[i++], 10) ? 1 : 0,
                parseInt(toks[i++], 10) ? 1 : 0,
                rel ? x + parseFloat(toks[i++]) : parseFloat(toks[i++]),
                rel ? y + parseFloat(toks[i++]) : parseFloat(toks[i++]));
            break;
          case 'z': case 'Z':
            x = sx; y = sy;
            cur = null;
            cmd = null;  // 'z z' repeats are no-ops; a bare number after
            break;         // 'z' is invalid path data
        }
      }
      return subpaths;
    }

    // Sutherland–Hodgman clip of a polygon against an axis-aligned rect.
    function clipPolyToRect(pts, x0, y0, x1, y1) {
      function inside(p, edge) {
        switch (edge) {
          case 0: return p[0] >= x0;
          case 1: return p[1] >= y0;
          case 2: return p[0] <= x1;
          default: return p[1] <= y1;
        }
      }
      function cross(a, b, edge) {
        if (edge === 0 || edge === 2) {
          var xv = (edge === 0) ? x0 : x1;
          var t = (xv - a[0]) / (b[0] - a[0]);
          return [xv, a[1] + t * (b[1] - a[1])];
        }
        var yv = (edge === 1) ? y0 : y1;
        var t2 = (yv - a[1]) / (b[1] - a[1]);
        return [a[0] + t2 * (b[0] - a[0]), yv];
      }
      var out = pts;
      for (var edge = 0; edge < 4; ++edge) {
        var inp = out;
        out = [];
        if (inp.length === 0)
          break;
        for (var i = 0; i < inp.length; ++i) {
          var a = inp[i], b = inp[(i + 1) % inp.length];
          var ai = inside(a, edge), bi = inside(b, edge);
          if (ai)
            out.push(a);
          if (ai !== bi)
            out.push(cross(a, b, edge));
        }
      }
      return out;
    }

    // ----------------------------------------------------------------
    // layout(e, x, y, scale, align): place element \a e whose baseline
    // origin is (x, y) at font scale \a scale; returns its box.
    // ----------------------------------------------------------------
    function layout(e, x, y, scale, align) {
      if (e.type !== 'el')
        return { w: 0, top: 0, depth: 0 };

      var cls = e.attrs['class'] || '';
      var classes = cls.split(/\s+/);
      var style = parseStyle(e.attrs['style']);
      scale = scale * sizingFactor(cls);

      var marginLeft = emLen(style, 'margin-left', st.unit) || 0;
      var marginRight = emLen(style, 'margin-right', st.unit) || 0;
      var paddingLeft = emLen(style, 'padding-left', st.unit) || 0;

      // --- raw <svg> (radical tails) --------------------------------
      if (e.name === 'svg') {
        var w = (parseFloat(e.attrs['width']) || 0) * scale;
        var h = (parseFloat(e.attrs['height']) || 0) * scale;
        drawSvg(e, scale, x + marginLeft, y - h, w, h);
        return { w: w + marginLeft + marginRight, top: h, depth: 0 };
      }

      // --- fixed-width spacers and struts ----------------------------
      if (classes.indexOf('mspace') >= 0)
        return { w: marginLeft + marginRight, top: 0, depth: 0 };
      if (classes.indexOf('nulldelimiter') >= 0)
        return { w: 0.12 * scale, top: 0, depth: 0 };
      if (classes.indexOf('arraycolsep') >= 0)
        return { w: emLen(style, 'width', st.unit) || 0, top: 0, depth: 0 };
      if (classes.indexOf('pstrut') >= 0 || classes.indexOf('katex-strut') >= 0
          || classes.indexOf('vlist-s') >= 0)
        return { w: 0, top: 0, depth: 0 };

      // --- vlist: items positioned by `top` offsets -------------------
      if (classes.indexOf('vlist-t') >= 0) {
        var items = collectVlistItems(e);
        if (items.length === 0)
          return flow(e, x, y, scale, align);

        // KaTeX's vlist contract (buildCommon.ts makeVList): each item is
        // `top: -(pstrutSize + currPos + depth)`, and its pstrut (an inline
        // block of height pstrutSize) puts the item's line-box baseline
        // pstrutSize below the item's top edge. Both resolve to: the item
        // content's baseline sits at -(pstrutSize + top) relative to the
        // vlist's baseline (positive = up). Items are centred on the vlist
        // content width (text-align: center).
        // "Stretch" items span the full vlist width: .hide-tail (radical
        // svg) and the CSS-border rules (.frac-line & friends).
        var boxes = [];
        var vlistW = 0;
        for (var i = 0; i < items.length; ++i) {
          var it = items[i];
          var istyle = parseStyle(it.attrs['style']);
          var top = emLen(istyle, 'top', st.unit);
          if (isNaN(top))
            top = 0;
          var pstrut = 0;
          for (var k = 0; k < it.children.length; ++k)
            if (it.children[k].type === 'el'
                && (it.children[k].attrs['class'] || '').indexOf('pstrut') >= 0) {
              pstrut = emLen(parseStyle(it.children[k].attrs['style']), 'height', st.unit);
              break;
            }
          if (isNaN(pstrut))
            pstrut = 0;
          var kind = stretchKind(it);
          var box = kind ? { w: 0, top: 0, depth: 0 } : flowMeasure(it, scale);
          if (!kind)
            vlistW = Math.max(vlistW, box.w);
          boxes.push({ off: (pstrut + top) * scale, box: box, kind: kind, el: it });
        }
        // Place. 
        var hi = 0, lo = 0;
        for (var j = 0; j < boxes.length; ++j) {
          var b = boxes[j];
          var w = b.kind ? vlistW : b.box.w;
          var dx = align === 'left' ? 0
                  : align === 'right' ? vlistW - w
                  : (vlistW - w) / 2;
          var baseY = y + b.off;
          if (b.kind === 'tail') {
            var tail = null;
            for (var k2 = 0; k2 < b.el.children.length; ++k2)
              if (b.el.children[k2].type === 'el'
                  && (b.el.children[k2].attrs['class'] || '').indexOf('hide-tail') >= 0)
                tail = b.el.children[k2];
            var pathEl = tail ? svgOf(tail) : null;
            if (pathEl) {
              // The slice box is the .hide-tail span's CSS box, not the
              // svg's attributes (KaTeX emits width="400em"
              // height="0.1em" viewBox="0 0 400 100" and lets
              // ".katex svg { width: 100%; height: inherit }" stretch it
              // into the span, whose inline height is the real one).
              var h = emLen(parseStyle(tail ? tail.attrs['style'] : null),
                            'height', st.unit);
              if (isNaN(h) || h <= 0)
                h = parseFloat(pathEl.attrs['height']) || 0;
              h *= scale;
              drawSvg(pathEl, scale, x + dx, baseY - h, w, h);
            }
          } else if (b.kind === 'line') {
            // CSS border-bottom of the rule span, at the item's baseline.
            var line = null;
            for (var k3 = 0; k3 < b.el.children.length; ++k3)
              if (b.el.children[k3].type === 'el'
                  && stretchKind(b.el.children[k3]) === 'line')
                line = b.el.children[k3];
            var th = emLen(parseStyle(line ? line.attrs['style'] : null),
                           'border-bottom-width', st.unit);
            if (isNaN(th))
              th = 0.04;
            st.out.push({ t: 'rect', x: x + dx, y: baseY, w: vlistW, h: th * scale });
          } else {
            flowAt(b.el, x + dx, baseY, scale, 'center');
          }
          hi = Math.max(hi, -b.off + b.box.top);
          lo = Math.max(lo, b.off + b.box.depth);
        }
        return { w: vlistW + marginLeft + marginRight, top: hi, depth: lo };
      }

      // --- matrix / array: columns of vlists on a shared baseline -----
      if (classes.indexOf('mtable') >= 0) {
        var cx = x + marginLeft;
        var top = 0, depth = 0;
        var seps = [];
        for (var i2 = 0; i2 < e.children.length; ++i2) {
          var c = e.children[i2];
          if (c.type !== 'el')
            continue;
          var ccls = c.attrs['class'] || '';
          if (ccls.indexOf('vertical-separator') >= 0) {
            seps.push(cx + 0.01 * scale);
            cx += 0.02 * scale;
            continue;
          }
          if (ccls.indexOf('arraycolsep') >= 0) {
            cx += emLen(parseStyle(c.attrs['style']), 'width', st.unit) || 0;
            continue;
          }
          var colAlign = 'center';
          if (ccls.indexOf('col-align-l') >= 0)
            colAlign = 'left';
          else if (ccls.indexOf('col-align-r') >= 0)
            colAlign = 'right';
          var colW = 0;
          for (var j2 = 0; j2 < c.children.length; ++j2) {
            var r = c.children[j2];
            if (r.type === 'el' && ((r.attrs['class'] || '').indexOf('vlist-t') >= 0)) {
              var box = layout(r, cx, y, scale, colAlign);
              colW = Math.max(colW, box.w);
              top = Math.max(top, box.top);
              depth = Math.max(depth, box.depth);
            }
          }
          cx += colW;
        }
        for (var s2 = 0; s2 < seps.length; ++s2)
          st.out.push({ t: 'rect', x: seps[s2], y: y - top, w: 0.02 * scale, h: top + depth });
        return { w: cx - x + marginRight, top: top, depth: depth };
      }

      // Margins are outside the box, padding inside: the content starts at
      // x + marginLeft + paddingLeft and the reported width covers the whole
      // margin box (CSS box model). Applies to leaves and element children
      // alike (e.g. the radical's radicand span carries padding-left:1em,
      // which keeps the text clear of the radical's hook).
      var txt = textOf(e);
      if (txt) {
        var font = fontOf(e);
        var mb = measure(txt, font, scale);
        st.out.push({
          t: 'text', x: x + marginLeft + paddingLeft, y: y,
          text: txt, px: scale * unit, top: mb.top, depth: mb.depth, w: mb.w,
          family: font.family, style: font.style, weight: font.weight
        });
        return { w: mb.w + marginLeft + paddingLeft + marginRight,
                 top: mb.top, depth: mb.depth };
      }

      var inner = flow(e, x + marginLeft + paddingLeft, y, scale, align);
      return { w: inner.w + marginLeft + paddingLeft + marginRight,
               top: inner.top, depth: inner.depth };
    }

    // Inline flow: children left-to-right on a shared baseline.
    function flow(e, x, y, scale, align) {
      var cx = x;
      var top = 0, depth = 0;
      for (var i = 0; i < e.children.length; ++i) {
        var b = layout(e.children[i], cx, y, scale, align);
        cx += b.w;
        top = Math.max(top, b.top);
        depth = Math.max(depth, b.depth);
      }
      return { w: cx - x, top: top, depth: depth };
    }

    function flowAt(e, x, y, scale, align) {
      return flow(e, x, y, scale, align);
    }

    // Flow layout without drawing (bounding box only).
    function flowMeasure(e, scale) {
      var saved = st.out;
      st.out = [];
      var box = flow(e, 0, 0, scale, 'center');
      st.out = saved;
      return box;
    }

    // 'tail' (radical svg), 'line' (CSS-border rule) or '' for a regular
    // vlist item: \a e (or one of its direct children) carries the marker.
    function stretchKind(e) {
      function clsOf(el) { return el.type === 'el' ? (el.attrs['class'] || '') : ''; }
      function kindOf(cls) {
        if (cls.indexOf('hide-tail') >= 0) return 'tail';
        if (cls.indexOf('frac-line') >= 0 || cls.indexOf('overline-line') >= 0
            || cls.indexOf('underline-line') >= 0)
          return 'line';
        return '';
      }
      var k = kindOf(clsOf(e));
      if (k)
        return k;
      for (var i = 0; i < e.children.length; ++i)
        if ((k = kindOf(clsOf(e.children[i]))))
          return k;
      return '';
    }

    // The .vlist-r > .vlist > span[top] items of a .vlist-t.
    function collectVlistItems(e) {
      var items = [];
      for (var i = 0; i < e.children.length; ++i) {
        var c = e.children[i];
        if (c.type !== 'el' || (c.attrs['class'] || '').indexOf('vlist-r') < 0)
          continue;
        for (var j = 0; j < c.children.length; ++j) {
          var v = c.children[j];
          if (v.type !== 'el' || (v.attrs['class'] || '').indexOf('vlist') < 0)
            continue;
          for (var k = 0; k < v.children.length; ++k)
            if (v.children[k].type === 'el')
              items.push(v.children[k]);
        }
      }
      return items;
    }

    // Top level.
    flow(htmlRoot, 0, 0, 1, 'center');
    if (st.out.length === 0)
      return null;

    // Bounds.
    var minX = 1e9, minY = 1e9, maxX = -1e9, maxY = -1e9;
    function extend(x1, y1, x2, y2) {
      minX = Math.min(minX, x1);
      minY = Math.min(minY, y1);
      maxX = Math.max(maxX, x2);
      maxY = Math.max(maxY, y2);
    }
    for (var i = 0; i < st.out.length; ++i) {
      var p = st.out[i];
      if (p.t === 'text')
        extend(p.x, p.y - p.top, p.x + p.w, p.y + p.depth);
      else if (p.t === 'rect')
        extend(p.x, p.y, p.x + p.w, p.y + p.h);
      else if (p.t === 'poly') {
        for (var j = 0; j < p.pts.length; ++j)
          extend(p.pts[j][0], p.pts[j][1], p.pts[j][0], p.pts[j][1]);
      }
    }
    if (minX > maxX || isNaN(minX))
      return null;

    var pad = 0.1;
    var wpx = (maxX - minX + 2 * pad) * unit;
    var hpx = (maxY - minY + 2 * pad) * unit;
    var ox = -minX + pad;
    var oy = -minY + pad;

    var svg = [];
    svg.push('<svg xmlns="http://www.w3.org/2000/svg" width="' + fmt(wpx) + '" height="' + fmt(hpx)
      + '" viewBox="0 0 ' + fmt(wpx) + ' ' + fmt(hpx) + '">');
    for (var i2 = 0; i2 < st.out.length; ++i2) {
      var p2 = st.out[i2];
      if (p2.t === 'text') {
        svg.push('<text x="' + fmt((p2.x + ox) * unit) + '" y="' + fmt((p2.y + oy) * unit) + '"'
          + ' font-family="' + p2.family + '"'
          + (p2.style !== 'normal' ? ' font-style="' + p2.style + '"' : '')
          + (p2.weight !== 400 ? ' font-weight="' + p2.weight + '"' : '')
          + ' font-size="' + fmt(p2.px) + '" fill="' + color + '">' + esc(p2.text) + '</text>');
      } else if (p2.t === 'rect') {
        svg.push('<rect x="' + fmt((p2.x + ox) * unit) + '" y="' + fmt((p2.y + oy) * unit)
          + '" width="' + fmt(p2.w * unit) + '" height="' + fmt(p2.h * unit) + '" fill="' + color + '"/>');
      } else if (p2.t === 'poly') {
        var dstr = 'M' + fmt((p2.pts[0][0] + ox) * unit) + ' ' + fmt((p2.pts[0][1] + oy) * unit);
        for (var q2 = 1; q2 < p2.pts.length; ++q2)
          dstr += 'L' + fmt((p2.pts[q2][0] + ox) * unit) + ' ' + fmt((p2.pts[q2][1] + oy) * unit);
        svg.push('<path d="' + dstr + 'Z" fill="' + color + '"/>');
      }
    }
    svg.push('</svg>');

    return {
      svg: svg.join(''),
      // Unrounded px: the caller re-boxes the SVG and inserts it with
      // sub-pixel image dimensions, so integer rounding here would show up
      // as a visible baseline offset.
      width: wpx,
      height: hpx,
      baseline: oy * unit
    };
  }

  g.__katexToSvg = katexToSvg;
})(typeof globalThis !== 'undefined' ? globalThis : this);
