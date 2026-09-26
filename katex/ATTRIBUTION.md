# Attribution

This directory vendors code from third-party projects:

## KaTeX (katex/katex.min.js)

KaTeX 0.18.9, https://github.com/KaTeX/KaTeX

Copyright (c) 2013-2016 The KaTeX authors

Licensed under the MIT License (see LICENSE):

    Permission is hereby granted, free of charge, to any person obtaining a copy
    of this software and associated documentation files (the "Software"), to deal
    in the Software without restriction, including without limitation the rights
    to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
    copies of the Software, and to permit persons to whom the Software is
    furnished to do so, subject to the following conditions:

    The above copyright notice and this permission notice shall be included in all
    copies or substantial portions of the Software.

    THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
    IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
    FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
    AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
    LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
    OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
    SOFTWARE.

The browser bundle (dist/katex.min.js) is used via `katex.renderToString()`.
The TrueType font files (fonts/, dist/fonts) are bundled as resources and
registered as application fonts so the SVG glyphs use KaTeX's own faces.

Note: the shipped font files declare `fsSelection = 0x40` (regular) for every
face, which makes font matchers (e.g. Qt's, via FreeType) treat the Italic and
Bold files as regular faces and synthesize fake styles. The bundled copies
have their OS/2 `fsSelection` bits set to match the subfamily (italic: 0x01,
bold: 0x20, bold italic: 0x21); no other bytes were changed.

## katex2svg.js

Written for this plugin (see its header comment). It runs KaTeX in the
QuickJS engine and flattens KaTeX's HTML output into a standalone SVG,
following the layout rules of KaTeX's own stylesheet (including its
class-to-font-family mapping). No third-party code.

## quickjs

The JavaScript engine used to run the above is quickjs-ng, which is part of
Qt Creator itself (src/libs/3rdparty/quickjs, MIT license) and linked via the
QtCreator::quickjsng CMake target.
