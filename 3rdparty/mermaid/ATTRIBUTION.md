# Attribution

This directory vendors code from third-party projects:

## mermaid.js (3rdparty/mermaid/mermaid.min.js)

Mermaid v12.0.0, https://github.com/mermaid-js/mermaid

Copyright (c) 2014 - 2025 Knut Sveidqvist and Mermaid contributors

Licensed under the MIT License:

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

## domshim.js

Written for this plugin (see its header comment). No third-party code.

## quickjs

The JavaScript engine used to run the above is quickjs-ng, which is part of
Qt Creator itself (src/libs/3rdparty/quickjs, MIT license) and linked via the
QtCreator::quickjsng CMake target.
