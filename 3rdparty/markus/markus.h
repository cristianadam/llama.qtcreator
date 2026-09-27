#ifndef MARKUS_H_
#define MARKUS_H_

#include <algorithm>
#include <array>
#include <bit>
#include <cctype>
#include <charconv>
#include <cstdint>
#include <cstring>
#include <deque>
#include <format>
#include <functional>
#include <list>
#include <map>
#include <memory>
#include <memory_resource>
#include <ranges>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

namespace markus {

// =============================================================================
// Memory Management / Allocation Model
// =============================================================================
// All parser containers are std::pmr types using the default PMR resource
// (std::pmr::get_default_resource(), i.e. new/delete). Every allocation is
// owned by the Document (or the returned std::pmr::string) and is freed when
// that object is destroyed - memory does not accumulate across Parse() calls.
// The one exception is transient parse state (container contents re-parsed by
// nested parsers, their line tables, paragraph text awaiting inline parsing):
// Parse() bump-allocates it from a std::pmr::monotonic_buffer_resource that is
// released before Parse() returns; nothing in the Document refers to it.
//
// A Document is self-contained: every string_view it exposes (Text and
// HtmlInline nodes, decoded entities) refers to storage owned by the Document
// itself (Document::string_storage), so the original Markdown input does NOT
// need to outlive the Document. Two Documents are independent; holding several
// at once is safe.
//
// Note: callers may install a process-wide default PMR resource via
// std::pmr::set_default_resource(); the containers above will then allocate
// from it, and its lifetime semantics apply to Documents as well.

// Lightweight result type optimized for parser functions.
// Replaces std::optional<T> to avoid overhead from type tracking
// and enable better compiler optimization for in-place construction.
// Key benefit: avoids double-check pattern (has_value + dereference)
// and enables emplace_back with in_place_type for variant containers.
template <typename T>
struct Result {
  bool has_value = false;
  alignas(T) unsigned char storage[sizeof(T)]{};

  Result() = default;
  Result(const Result&) = delete;
  Result& operator=(const Result&) = delete;

  // Moves are exception-safe: the value is moved into a temporary first, so a
  // throwing move leaves both operands in a valid state (this one empty).
  Result(Result&& other) : has_value(false) {
    if (other.has_value) {
      T tmp = std::move(*reinterpret_cast<T*>(other.storage));
      new (storage) T(std::move(tmp));
      other.has_value = false;
      has_value = true;
    }
  }

  Result& operator=(Result&& other) {
    if (this != &other) {
      if (other.has_value) {
        T tmp = std::move(*reinterpret_cast<T*>(other.storage));
        if (has_value) {
          reinterpret_cast<T*>(storage)->~T();
          has_value = false;
        }
        new (storage) T(std::move(tmp));
        other.has_value = false;
        has_value = true;
      } else if (has_value) {
        reinterpret_cast<T*>(storage)->~T();
        has_value = false;
      }
    }
    return *this;
  }

  Result(T&& value) {
    new (storage) T(std::move(value));
    has_value = true;
  }

  ~Result() {
    if (has_value) {
      reinterpret_cast<T*>(storage)->~T();
    }
  }

  template <typename... Args>
  static Result emplace(Args&&... args) {
    Result r;
    new (r.storage) T(std::forward<Args>(args)...);
    r.has_value = true;
    return r;
  }

  static Result none() { return Result{}; }

  explicit operator bool() const { return has_value; }
  T& operator*() { return *reinterpret_cast<T*>(storage); }
  const T& operator*() const { return *reinterpret_cast<const T*>(storage); }
  T* operator->() { return reinterpret_cast<T*>(storage); }
  const T* operator->() const { return reinterpret_cast<const T*>(storage); }
  T& value() { return *reinterpret_cast<T*>(storage); }
  const T& value() const { return *reinterpret_cast<const T*>(storage); }
};

// =============================================================================
// Forward Declarations
// =============================================================================

struct Document;
struct Paragraph;
struct Heading;
struct ThematicBreak;
struct CodeBlock;
struct DetailsBlock;
struct HtmlBlock;
struct BlockQuote;
struct List;
struct ListItem;
struct Table;
struct Text;
struct SoftBreak;
struct HardBreak;
struct Code;
struct Emphasis;
struct Strong;
struct Strikethrough;
struct Link;
struct Image;
struct HtmlInline;

// =============================================================================
// AST Node Types
// =============================================================================

// Node type enumeration for debugging and introspection
enum class NodeType {
  kDocument,
  kParagraph,
  kHeading,
  kThematicBreak,
  kCodeBlock,
  kDetailsBlock,
  kHtmlBlock,
  kBlockQuote,
  kList,
  kListItem,
  kTable,
  kText,
  kSoftBreak,
  kHardBreak,
  kCode,
  kEmphasis,
  kStrong,
  kStrikethrough,
  kMath,
  kLink,
  kImage,
  kHtmlInline,
};

// Convert NodeType to string for debugging
inline std::string_view NodeTypeToString(NodeType type) {
  switch (type) {
    case NodeType::kDocument:
      return "Document";
    case NodeType::kParagraph:
      return "Paragraph";
    case NodeType::kHeading:
      return "Heading";
    case NodeType::kThematicBreak:
      return "ThematicBreak";
    case NodeType::kCodeBlock:
      return "CodeBlock";
    case NodeType::kDetailsBlock:
      return "DetailsBlock";
    case NodeType::kHtmlBlock:
      return "HtmlBlock";
    case NodeType::kBlockQuote:
      return "BlockQuote";
    case NodeType::kList:
      return "List";
    case NodeType::kListItem:
      return "ListItem";
    case NodeType::kTable:
      return "Table";
    case NodeType::kText:
      return "Text";
    case NodeType::kSoftBreak:
      return "SoftBreak";
    case NodeType::kHardBreak:
      return "HardBreak";
    case NodeType::kCode:
      return "Code";
    case NodeType::kEmphasis:
      return "Emphasis";
    case NodeType::kStrong:
      return "Strong";
    case NodeType::kStrikethrough:
      return "Strikethrough";
    case NodeType::kMath:
      return "Math";
    case NodeType::kLink:
      return "Link";
    case NodeType::kImage:
      return "Image";
    case NodeType::kHtmlInline:
      return "HtmlInline";
  }
  return "Unknown";
}

// =============================================================================
// Node ID Types - indices into node pools for compact storage
// =============================================================================

using InlineNodeId = uint32_t;
using BlockNodeId = uint32_t;
constexpr InlineNodeId kInvalidInlineNodeId = ~uint32_t(0);
constexpr BlockNodeId kInvalidBlockNodeId = ~uint32_t(0);

// =============================================================================
// Inline Node Definitions
// =============================================================================

struct Text {
  // Non-owning view. Always refers to storage owned by the enclosing
  // Document (its string_storage), never to the original Markdown input.
  // Valid for the lifetime of that Document.
  std::string_view content;

  explicit Text(std::string_view c) : content(c) {}
  Text() = default;
};

struct SoftBreak {};

struct HardBreak {};

struct Code {
  std::pmr::string content;

  explicit Code(std::pmr::string c) : content(std::move(c)) {}
  Code() = default;
};

struct Emphasis {
  std::pmr::vector<InlineNodeId> children;
};

struct Strong {
  std::pmr::vector<InlineNodeId> children;
};

struct Strikethrough {
  std::pmr::vector<InlineNodeId> children;
};

// LaTeX math span (the `latexmath` extension, mirroring md4c's
// MD_FLAG_LATEXMATHSPANS): text wrapped in `$...$` (inline math) or
// `$$...$$` (display math). The content is verbatim - no inline parsing
// (emphasis, code spans, links, ...) happens inside a math span. `content`
// holds the raw text between the delimiters with every run of whitespace
// collapsed to a single space (a soft line break becomes a space, as for
// regular text). `display` is true when the span was delimited by `$$`.
struct Math {
  std::pmr::string content;
  bool display = false;

  Math() = default;
  Math(std::pmr::string c, bool d) : content(std::move(c)), display(d) {}
};

struct Link {
  std::pmr::string destination;
  std::pmr::string title;
  std::pmr::vector<InlineNodeId> children;
};

struct Image {
  std::pmr::string destination;
  std::pmr::string title;
  std::pmr::string alt_text;
};

struct HtmlInline {
  // Non-owning view, same lifetime contract as Text::content (owned by the
  // enclosing Document, never the original Markdown input).
  std::string_view content;

  explicit HtmlInline(std::string_view c) : content(c) {}
  HtmlInline() = default;
};

// Variant type for inline content
using InlineNode = std::variant<Text, SoftBreak, HardBreak, Code, Emphasis,
                                Strong, Strikethrough, Math, Link, Image,
                                HtmlInline>;

// =============================================================================
// Block Node Definitions
// =============================================================================

struct Paragraph {
  std::pmr::vector<InlineNodeId> children;
  std::pmr::string raw_content;  // Temporary storage for inline parsing
};

struct Heading {
  int level = 1;  // 1-6
  std::pmr::vector<InlineNodeId> children;
  std::pmr::string raw_content;  // Temporary storage for inline parsing

  Heading() = default;
  Heading(int lvl, std::pmr::string content)
      : level(lvl), raw_content(std::move(content)) {}
};

struct ThematicBreak {};

struct CodeBlock {
  std::pmr::string info_string;  // Language hint (e.g., "cpp", "python")
  std::pmr::string content;
  bool is_fenced = false;
  char fence_char = 0;  // '`' or '~' when fenced, 0 for indented code blocks

  CodeBlock() = default;
  CodeBlock(std::pmr::string info, std::pmr::string content, bool fenced,
            char fence = 0)
      : info_string(std::move(info)),
        content(std::move(content)),
        is_fenced(fenced),
        fence_char(fence) {}
};

// A GitHub-style collapsible `<details>` section. Produced when a type-6 HTML
// block starts with a `<details>` tag; the block then consumes lines until the
// matching `</details>` line (blank lines do not terminate it, unlike other
// type-6 HTML blocks). `summary` holds the block nodes parsed from the optional
// `<summary>` child (the text between the end of the opening `<summary ...>`
// tag and the `</summary>` line); `children` are the block nodes parsed from
// the section body (the text between the end of `</summary>` (or the opening
// tag) and the `</details>` line). Both are markdown, stored in the document's
// block pool, so the summary supports the same block/inline markup as the
// body. `attributes` are the (name, value) pairs of the opening
// `<details ...>` tag in document order (boolean attributes have an empty
// value), so consumers can react to markup like `<details data-tool>` without
// lossy workarounds.
struct DetailsBlock {
  std::pmr::vector<BlockNodeId> summary;
  std::pmr::vector<BlockNodeId> children;
  bool closed = false;  // false when the input ended before </details>
  std::pmr::vector<std::pair<std::pmr::string, std::pmr::string>> attributes;
};

struct HtmlBlock {
  std::pmr::string content;
  int block_type = 0;  // CommonMark HTML block type (1-7)

  HtmlBlock() = default;
  HtmlBlock(std::pmr::string content, int type)
      : content(std::move(content)), block_type(type) {}
};

struct ListItem {
  std::pmr::vector<BlockNodeId> children;
  bool is_tight = true;

  // GFM `tasklist` extension. Set when this item's first line began with a
  // `[ ]`/`[x]`/`[X]` marker (the marker is consumed and the item is rendered
  // with a checkbox). `tasklist_checked` is true for `[x]`/`[X]`.
  bool is_tasklist = false;
  bool tasklist_checked = false;
};

struct List {
  bool is_ordered = false;
  int start = 1;           // Starting number for ordered lists
  char delimiter = '.';    // '.' or ')' for ordered lists
  char bullet_char = '-';  // '-', '+', or '*' for unordered lists
  bool is_tight = true;
  std::pmr::vector<ListItem> items;

  List() = default;
  List(bool ordered, int start_num, char delim, char bullet, bool tight,
       std::pmr::vector<ListItem> items)
      : is_ordered(ordered),
        start(start_num),
        delimiter(delim),
        bullet_char(bullet),
        is_tight(tight),
        items(std::move(items)) {}
};

struct BlockQuote {
  std::pmr::vector<BlockNodeId> children;

  BlockQuote() = default;
  explicit BlockQuote(std::pmr::vector<BlockNodeId> children)
      : children(std::move(children)) {}
};

// Column alignment for GFM tables (GFM `table` extension).
enum class TableAlign : uint8_t {
  kNone = 0,
  kLeft = 1,
  kCenter = 2,
  kRight = 3,
};

struct TableCell {
  std::pmr::string raw_content;  // Cell text; inlines parsed in the 3rd pass
  std::pmr::vector<InlineNodeId> children;

  TableCell() = default;
  explicit TableCell(std::pmr::string content)
      : raw_content(std::move(content)) {}
};

struct TableRow {
  bool is_header = false;
  std::pmr::vector<TableCell>
      cells;  // Always padded to the table's column count
};

struct Table {
  std::pmr::vector<TableAlign> alignments;  // Size == number of columns
  std::pmr::vector<TableRow> rows;          // rows[0] is the header row
};

// Variant type for block content
using BlockNode = std::variant<Paragraph, Heading, ThematicBreak, CodeBlock,
                               DetailsBlock, HtmlBlock, BlockQuote, List,
                               ListItem, Table>;

// Link reference definitions: normalized label -> (destination, title).
// An ordered map (with transparent string_view lookup) keeps both insertion
// and lookup O(log n); a sorted vector made each insertion O(n), so many
// definitions parsed quadratically.
using LinkRefMap =
    std::pmr::map<std::pmr::string,
                  std::pair<std::pmr::string, std::pmr::string>, std::less<>>;

// Parsed Markdown AST.
//
// Lifetime: a Document is self-contained. All inline text (Text and
// HtmlInline nodes) and decoded entities reference storage owned by this
// Document (see string_storage below), so the original Markdown input does
// NOT need to outlive the Document. All memory is released when the Document
// is destroyed; holding multiple Documents simultaneously is safe.
struct Document {
  std::pmr::vector<BlockNode> children;

  // Link reference definitions (label -> (destination, title))
  LinkRefMap link_references;

  // Storage for decoded strings (entities, escapes) that inline nodes view into
  // Using deque to avoid reference invalidation on growth
  std::deque<std::pmr::string> string_storage;

  // Node pools for compact storage - nodes are referenced by ID (index).
  // inline_nodes may also hold a few unreferenced slots (nodes dropped while
  // resolving emphasis, links and math); walk the tree from the block nodes
  // rather than iterating the pool.
  std::pmr::vector<InlineNode> inline_nodes;
  std::pmr::vector<BlockNode> block_nodes;

  Document() = default;
  Document(Document&&) = default;
  Document& operator=(Document&&) = default;

  // Copying is a deep copy: the Text/HtmlInline views of the copy are
  // re-pointed into the copy's own string_storage, so the copy stays valid
  // after the original is destroyed. (A member-wise copy would leave them
  // viewing the original's storage.) Moving keeps the views valid as is,
  // since a deque move does not relocate its elements.
  Document(const Document& other)
      : children(other.children),
        link_references(other.link_references),
        string_storage(other.string_storage),
        inline_nodes(other.inline_nodes),
        block_nodes(other.block_nodes) {
    RebaseViews(other);
  }

  Document& operator=(const Document& other) {
    if (this != &other) *this = Document(other);
    return *this;
  }

  // Add a block to the pool and return its ID
  BlockNodeId AddBlock(BlockNode&& node) {
    BlockNodeId id = static_cast<BlockNodeId>(block_nodes.size());
    block_nodes.push_back(std::move(node));
    return id;
  }

 private:
  // After copying `source`, re-point every inline view that refers into one
  // of source's storage strings at the same offset of the corresponding copy.
  void RebaseViews(const Document& source) {
    struct Chunk {
      const char* begin;
      size_t size;
      size_t index;
    };
    std::vector<Chunk> chunks;
    chunks.reserve(source.string_storage.size());
    for (size_t i = 0; i < source.string_storage.size(); ++i) {
      const std::pmr::string& str = source.string_storage[i];
      if (!str.empty()) chunks.push_back({str.data(), str.size(), i});
    }
    std::sort(chunks.begin(), chunks.end(), [](const Chunk& a, const Chunk& b) {
      return std::less<const char*>()(a.begin, b.begin);
    });
    auto rebase = [&](std::string_view& view) {
      if (view.empty()) {
        view = std::string_view();
        return;
      }
      // Last chunk starting at or before the view.
      auto it = std::upper_bound(chunks.begin(), chunks.end(), view.data(),
                                 [](const char* p, const Chunk& c) {
                                   return std::less<const char*>()(p, c.begin);
                                 });
      if (it != chunks.begin()) {
        const Chunk& c = *std::prev(it);
        uintptr_t p = reinterpret_cast<uintptr_t>(view.data());
        uintptr_t b = reinterpret_cast<uintptr_t>(c.begin);
        size_t offset = static_cast<size_t>(p - b);
        if (p >= b && offset <= c.size && view.size() <= c.size - offset) {
          view = std::string_view(string_storage[c.index].data() + offset,
                                  view.size());
          return;
        }
      }
      // Not in the source's storage (not expected): give the copy its own.
      view = string_storage.emplace_back(view);
    };
    for (InlineNode& node : inline_nodes) {
      if (auto* text = std::get_if<Text>(&node)) {
        rebase(text->content);
      } else if (auto* html = std::get_if<HtmlInline>(&node)) {
        rebase(html->content);
      }
    }
  }
};

// =============================================================================
// Utility Functions
// =============================================================================

namespace detail {

using namespace std::literals;

// =============================================================================
// Resource Limits
// =============================================================================

// Maximum nesting depth of container blocks (block quotes, list items,
// <details> sections). Each level is parsed, inline-parsed and rendered
// recursively, so unbounded nesting lets a few KB of input such as `>>>>...`
// overflow the stack. Content nested deeper than this is kept as literal
// paragraph text instead of being parsed as further blocks.
inline constexpr int kMaxBlockNesting = 64;

// Maximum nesting depth of inline containers (emphasis, strong,
// strikethrough, links, images). Delimiters that would nest deeper are left
// as literal text, bounding the recursion of the inline parser, the renderer
// and the AST printer.
inline constexpr int kMaxInlineNesting = 128;

// Container blocks are parsed by copying their content (minus the container
// markers) and re-parsing the copy, so deeply nested input is copied once per
// level. The total copied during one parse is limited to this multiple of the
// input size (plus kNestedContentSlack); past it, container content is kept
// as literal paragraph text, as beyond kMaxBlockNesting. Ordinary documents
// copy only a few times their size.
inline constexpr size_t kMaxNestedContentFactor = 16;
inline constexpr size_t kNestedContentSlack = 64u << 10;

// Maximum number of empty cells a GFM table may add to pad short body rows
// out to the header's column count. A tiny input with a wide header and many
// one-cell rows otherwise expands into millions of cells (cmark-gfm limits
// this the same way). Once exceeded, the table ends and the remaining lines
// are parsed as ordinary blocks.
inline constexpr size_t kMaxTableAutoCompletedCells = 1u << 16;

// =============================================================================
// Compile-Time Utilities
// =============================================================================

// Generate escape table at compile time using constexpr
inline constexpr auto MakeEscapeTable() {
  std::array<uint8_t, 256> table{};
  // & (0x26) -> 1, < (0x3C) -> 2, > (0x3E) -> 3, " (0x22) -> 4
  table[0x26] = 1;  // &
  table[0x3C] = 2;  // <
  table[0x3E] = 3;  // >
  table[0x22] = 4;  // "
  return table;
}

inline constexpr auto kEscapeTableV2 = MakeEscapeTable();

// Generate hex digit table at compile time
inline constexpr auto MakeHexDigitTable() {
  std::array<char, 256> table{};
  for (int i = 0; i < 16; ++i) {
    table[i] = "0123456789ABCDEF"[i];
    table[i + 16] = "0123456789abcdef"[i];
    table[i + 32] = "0123456789ABCDEF"[i];
    table[i + 48] = "0123456789ABCDEF"[i];
  }
  return table;
}

inline constexpr auto kHexDigitTable = MakeHexDigitTable();

// Generate escape string lengths at compile time
inline constexpr auto MakeEscapeLens() {
  std::array<uint8_t, 5> lens{};
  lens[1] = 5;  // &amp;
  lens[2] = 4;  // &lt;
  lens[3] = 4;  // &gt;
  lens[4] = 6;  // &quot;
  return lens;
}

inline constexpr auto kEscapeLens = MakeEscapeLens();

// =============================================================================
// Optimized HTML Escaping - In-place variant (hot path)
// =============================================================================

// Writes escaped HTML directly to output buffer - eliminates temp string
// allocation. Scans 8 bytes at a time with a SWAR test for the four special
// characters ('&', '<', '>', '"'), then locates the exact byte with a short
// loop (endian-independent).
inline void EscapeHtmlTo(std::string_view text, std::pmr::string& out) {
  const size_t len = text.size();
  if (len == 0) return;
  const char* data = text.data();
  constexpr uint64_t kOnes = 0x0101010101010101ULL;
  constexpr uint64_t kHighs = 0x8080808080808080ULL;
  auto has_byte = [](uint64_t word, char c) {
    uint64_t v = word ^ (kOnes * static_cast<unsigned char>(c));
    return (v - kOnes) & ~v & kHighs;
  };
  size_t i = 0;

  while (i < len) {
    size_t start = i;
    // Skip whole 8-byte words that contain no special character
    while (i + 8 <= len) {
      uint64_t word;
      std::memcpy(&word, data + i, sizeof(word));
      if (has_byte(word, '&') | has_byte(word, '<') | has_byte(word, '>') |
          has_byte(word, '"')) {
        break;
      }
      i += 8;
    }
    // Locate the special character (within the flagged word, or in the tail)
    while (i < len && !kEscapeTableV2[static_cast<unsigned char>(data[i])]) {
      ++i;
    }

    // Batch copy safe span
    if (i > start) {
      out.append(data + start, i - start);
    }

    // Handle escaped character
    if (i < len) {
      uint8_t e = kEscapeTableV2[static_cast<unsigned char>(data[i])];
      switch (e) {
        case 1:
          out.append("&amp;");
          break;
        case 2:
          out.append("&lt;");
          break;
        case 3:
          out.append("&gt;");
          break;
        case 4:
          out.append("&quot;");
          break;
      }
      ++i;
    }
  }
}

// =============================================================================
// GFM tagfilter (Disallowed Raw HTML) extension
// =============================================================================

// The GFM `tagfilter` extension filters a fixed set of HTML tags out of raw
// HTML by replacing their leading '<' with the entity "&lt;", since these tags
// change how the surrounding rendered Markdown is interpreted (see the GFM
// spec, "Disallowed Raw HTML (extension)").
//
// Note: URL scheme filtering (e.g. blocking `javascript:` in link/image hrefs)
// is intentionally out of scope here, matching cmark-gfm/GFM, which also only
// percent-escape hrefs rather than inspect their scheme. Sanitising link
// schemes is a consumer / HTML-sanitiser concern.
namespace tagfilter {

// Tags filtered by the GFM tagfilter extension.
inline constexpr std::array<std::string_view, 9> kDisallowedTags = {
    "title",   "textarea", "style",  "xmp",       "iframe",
    "noembed", "noframes", "script", "plaintext"};

// Returns true if the tag starting at `data` (which must begin with '<') has
// the name `tagname`, following CommonMark tag grammar: an optional leading
// '/', the tag name (case-insensitive), then whitespace, '>', or '/>'.
inline bool IsTag(std::string_view data, std::string_view tagname) {
  if (data.size() < 3 || data[0] != '<') {
    return false;
  }
  size_t i = 1;
  if (data[i] == '/') {
    ++i;
  }
  size_t k = 0;  // index into tagname
  for (; i < data.size(); ++i, ++k) {
    if (k == tagname.size()) {
      break;
    }
    if (std::tolower(static_cast<unsigned char>(data[i])) !=
        static_cast<unsigned char>(tagname[k])) {
      return false;
    }
  }
  if (i == data.size()) {
    return false;
  }
  const unsigned char c = static_cast<unsigned char>(data[i]);
  if (std::isspace(c) || c == '>') {
    return true;
  }
  if (c == '/' && data.size() >= i + 2 && data[i + 1] == '>') {
    return true;
  }
  return false;
}

// Returns true if the tag starting at `data` is one of the disallowed GFM tags
// (i.e. its leading '<' must be escaped to '&lt;' when rendering).
inline bool IsDisallowedTag(std::string_view data) {
  for (std::string_view name : kDisallowedTags) {
    if (IsTag(data, name)) {
      return true;
    }
  }
  return false;
}

// Renders a raw HTML block, escaping the leading '<' of any disallowed GFM tag
// (matching cmark-gfm's `filter_html_block`). All other content is emitted
// verbatim.
inline void RenderFilteredHtmlBlock(std::string_view content,
                                    std::pmr::string& out) {
  const size_t len = content.size();
  size_t pos = 0;
  while (pos < len) {
    const size_t lt = content.find('<', pos);
    if (lt == std::string_view::npos) {
      out.append(content.substr(pos));
      break;
    }
    if (lt > pos) {
      out.append(content.substr(pos, lt - pos));
    }
    if (IsDisallowedTag(content.substr(lt))) {
      out.append("&lt;");
    } else {
      out.push_back('<');
    }
    pos = lt + 1;
  }
}

}  // namespace tagfilter

// =============================================================================
// Optimized URL Encoding - In-place variant
// =============================================================================

// URL safe character table (same as original kSafeTable)
inline constexpr auto MakeUrlSafeTable() {
  std::array<uint8_t, 256> table{};
  // Alphanumeric
  for (int i = '0'; i <= '9'; ++i) table[i] = 1;
  for (int i = 'a'; i <= 'z'; ++i) table[i] = 1;
  for (int i = 'A'; i <= 'Z'; ++i) table[i] = 1;
  // Safe chars: -_.~:/?#@!$&'()*+,;=%
  const char safe[] = "-_.~:/?#@!$&'()*+,;=%";
  for (char c : safe) table[static_cast<unsigned char>(c)] = 1;
  return table;
}

inline constexpr auto kSafeTable = MakeUrlSafeTable();

inline void EncodeUrlTo(std::string_view url, std::pmr::string& out) {
  size_t i = 0;
  const size_t len = url.size();

  while (i < len) {
    size_t start = i;
    // 8-wide unrolled scan for safe characters
    while (i + 7 < len) {
      if (!kSafeTable[static_cast<unsigned char>(url[i])]) break;
      if (!kSafeTable[static_cast<unsigned char>(url[i + 1])]) break;
      if (!kSafeTable[static_cast<unsigned char>(url[i + 2])]) break;
      if (!kSafeTable[static_cast<unsigned char>(url[i + 3])]) break;
      if (!kSafeTable[static_cast<unsigned char>(url[i + 4])]) break;
      if (!kSafeTable[static_cast<unsigned char>(url[i + 5])]) break;
      if (!kSafeTable[static_cast<unsigned char>(url[i + 6])]) break;
      if (!kSafeTable[static_cast<unsigned char>(url[i + 7])]) break;
      i += 8;
    }
    while (i < len) {
      if (!kSafeTable[static_cast<unsigned char>(url[i])]) break;
      ++i;
    }

    if (i > start) {
      out.append(url.data() + start, i - start);
    }

    if (i < len) {
      unsigned char c = static_cast<unsigned char>(url[i]);
      out += '%';
      out += kHexDigitTable[c >> 4];
      out += kHexDigitTable[c & 0xF];
      ++i;
    }
  }
}

// =============================================================================
// Integer formatting with std::to_chars (no allocation)
// =============================================================================

inline char* AppendIntToBuffer(int value, char* buf) {
  auto [ptr, ec] = std::to_chars(buf, buf + 12, value);
  return ptr;
}

// =============================================================================
// Helper Functions
// =============================================================================

inline size_t FindFirstSpecialChar(const char* data, size_t len,
                                   const uint8_t* special_table) {
  auto it = std::find_if(data, data + len, [special_table](char c) {
    return special_table[static_cast<unsigned char>(c)];
  });
  return static_cast<size_t>(it - data);
}

inline size_t FindFirstUnsafeChar(const char* data, size_t len,
                                  const uint8_t* safe_table) {
  auto it = std::find_if(data, data + len, [safe_table](char c) {
    return !safe_table[static_cast<unsigned char>(c)];
  });
  return static_cast<size_t>(it - data);
}

// Check if a span contains only blank characters (space, tab, \r, \n)
// C++20: uses std::ranges::all_of for clean short-circuiting
inline bool IsSpanBlank(std::string_view sv) {
  return std::ranges::all_of(sv, [](char c) {
    unsigned char uc = static_cast<unsigned char>(c);
    return uc == ' ' || uc == '\t' || uc == '\n' || uc == '\r';
  });
}

// Legacy overload for raw pointer + length (deprecated - use string_view
// version)
inline bool IsSpanBlank(const char* data, size_t len) {
  return std::ranges::all_of(std::string_view(data, len), [](char c) {
    unsigned char uc = static_cast<unsigned char>(c);
    return uc == ' ' || uc == '\t' || uc == '\n' || uc == '\r';
  });
}

inline std::pair<size_t, bool> ScanForLinesAndNulls(const char* data,
                                                    size_t len) {
  size_t line_count = 0;
  bool has_nulls = false;
  for (size_t i = 0; i < len; ++i) {
    char c = data[i];
    if (c == '\n') {
      ++line_count;
    } else if (c == '\r') {
      ++line_count;
      if (i + 1 < len && data[i + 1] == '\n') {
        ++i;  // Skip the \n in \r\n
      }
    } else if (c == '\0') {
      has_nulls = true;
    }
  }
  return {line_count, has_nulls};
}

// Find next line ending (\n or \r) starting from pos
// Returns position of line ending, or len if not found
inline size_t FindNextLineEnding(const char* data, size_t len, size_t start) {
  size_t i = start;
  // Process 8 bytes at a time
  for (; i + 8 <= len; i += 8) {
    // Check 8 bytes for line endings
    bool found = false;
    for (size_t j = 0; j < 8; ++j) {
      char c = data[i + j];
      if (c == '\n' || c == '\r') {
        found = true;
        break;
      }
    }
    if (found) {
      // Find exact position
      for (size_t j = i; j < i + 8 && j < len; ++j) {
        if (data[j] == '\n' || data[j] == '\r') {
          return j;
        }
      }
    }
  }
  // Handle remaining bytes
  for (; i < len; ++i) {
    if (data[i] == '\n' || data[i] == '\r') {
      return i;
    }
  }
  return len;
}

// Check if a character is ASCII punctuation
// Uses a 256-byte lookup table for O(1) performance
inline bool IsAsciiPunctuation(char c) {
  // Lookup table: 1 if ASCII punctuation, 0 otherwise
  // ASCII punctuation: 0x21-0x2F, 0x3A-0x40, 0x5B-0x60, 0x7B-0x7E
  static constexpr uint8_t kPunctuationTable[256] = {
      0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,  // 0x00-0x0F
      0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,  // 0x10-0x1F
      0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,  // 0x20-0x2F (! to /)
      0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 1,  // 0x30-0x3F (: to ?)
      1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,  // 0x40-0x4F (@)
      0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1,  // 0x50-0x5F ([ to _)
      1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,  // 0x60-0x6F (`)
      0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 0,  // 0x70-0x7F ({ to ~)
      0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,  // 0x80-0x8F
      0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,  // 0x90-0x9F
      0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,  // 0xA0-0xAF
      0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,  // 0xB0-0xBF
      0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,  // 0xC0-0xCF
      0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,  // 0xD0-0xDF
      0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,  // 0xE0-0xEF
      0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,  // 0xF0-0xFF
  };
  return kPunctuationTable[static_cast<unsigned char>(c)] != 0;
}

// Case-fold lookup table: maps each byte to its lowercase equivalent
// Replaces std::tolower() calls for O(1) case-insensitive comparisons
inline constexpr auto MakeCaseFoldTable() {
  std::array<uint8_t, 256> table{};
  for (int i = 0; i < 256; ++i) {
    table[i] = static_cast<uint8_t>(i);
    if (i >= 'A' && i <= 'Z') {
      table[i] = static_cast<uint8_t>(i + ('a' - 'A'));
    }
  }
  return table;
}

inline constexpr auto kCaseFoldTable = MakeCaseFoldTable();

inline uint8_t CaseFold(uint8_t c) { return kCaseFoldTable[c]; }

// Character classification lookup table: bit flags per byte
// Bit 0: isalpha, Bit 1: isdigit, Bit 2: isalnum, Bit 3: isxdigit
inline constexpr auto MakeCharClassTable() {
  std::array<uint8_t, 256> table{};
  for (int i = 0; i < 256; ++i) {
    uint8_t flags = 0;
    if ((i >= 'a' && i <= 'z') || (i >= 'A' && i <= 'Z')) flags |= 1;
    if (i >= '0' && i <= '9') flags |= 2;
    if (flags & 1 || (flags & 2)) flags |= 4;
    if ((i >= 'a' && i <= 'f') || (i >= 'A' && i <= 'F') ||
        (i >= '0' && i <= '9'))
      flags |= 8;
    table[i] = flags;
  }
  return table;
}

inline constexpr auto kCharClassTable = MakeCharClassTable();

inline bool IsAlpha(uint8_t c) { return (kCharClassTable[c] & 1) != 0; }
inline bool IsDigit(uint8_t c) { return (kCharClassTable[c] & 2) != 0; }
inline bool IsAlnum(uint8_t c) { return (kCharClassTable[c] & 4) != 0; }
inline bool IsXDigit(uint8_t c) { return (kCharClassTable[c] & 8) != 0; }

// Parse unsigned integer directly from string_view without temporary
// allocation. Values that would overflow uint32_t are clamped to UINT32_MAX
// (callers validate the range; e.g. entity code points map to U+FFFD).
inline bool ParseUint(std::string_view sv, uint32_t& out, int base) {
  if (sv.empty()) return false;
  uint32_t result = 0;
  bool overflowed = false;
  for (char c : sv) {
    int digit = 0;
    if (c >= '0' && c <= '9')
      digit = c - '0';
    else if (base == 16 && c >= 'a' && c <= 'f')
      digit = c - 'a' + 10;
    else if (base == 16 && c >= 'A' && c <= 'F')
      digit = c - 'A' + 10;
    else
      return false;
    if (digit >= base) return false;
    if (!overflowed) {
      const uint32_t d = static_cast<uint32_t>(digit);
      if (result > (UINT32_MAX - d) / static_cast<uint32_t>(base)) {
        overflowed = true;
        result = UINT32_MAX;
      } else {
        result = result * static_cast<uint32_t>(base) + d;
      }
    }
  }
  out = result;
  return true;
}

// Get the byte length of a UTF-8 character starting at the given byte
inline size_t Utf8CharLen(unsigned char c) {
  if ((c & 0x80) == 0) return 1;     // ASCII
  if ((c & 0xE0) == 0xC0) return 2;  // 110xxxxx
  if ((c & 0xF0) == 0xE0) return 3;  // 1110xxxx
  if ((c & 0xF8) == 0xF0) return 4;  // 11110xxx
  return 1;                          // Invalid, treat as single byte
}

// Decode UTF-8 code point at position, return code point and bytes consumed
inline std::pair<uint32_t, size_t> DecodeUtf8At(std::string_view s,
                                                size_t pos) {
  if (pos >= s.size()) [[unlikely]]
    return {0, 0};
  unsigned char c = static_cast<unsigned char>(s[pos]);
  if ((c & 0x80) == 0) {
    return {c, 1};
  }
  if ((c & 0xE0) == 0xC0 && pos + 1 < s.size()) {
    uint32_t cp = (c & 0x1F) << 6;
    cp |= (static_cast<unsigned char>(s[pos + 1]) & 0x3F);
    return {cp, 2};
  }
  if ((c & 0xF0) == 0xE0 && pos + 2 < s.size()) {
    uint32_t cp = (c & 0x0F) << 12;
    cp |= (static_cast<unsigned char>(s[pos + 1]) & 0x3F) << 6;
    cp |= (static_cast<unsigned char>(s[pos + 2]) & 0x3F);
    return {cp, 3};
  }
  if ((c & 0xF8) == 0xF0 && pos + 3 < s.size()) {
    uint32_t cp = (c & 0x07) << 18;
    cp |= (static_cast<unsigned char>(s[pos + 1]) & 0x3F) << 12;
    cp |= (static_cast<unsigned char>(s[pos + 2]) & 0x3F) << 6;
    cp |= (static_cast<unsigned char>(s[pos + 3]) & 0x3F);
    return {cp, 4};
  }
  return {c, 1};
}

// Fast ASCII path for Unicode punctuation - inlined constexpr for compile-time
// evaluation of the most common case (ASCII characters make up >95% of text)
// Matches the original kPunctuationTable exactly
inline constexpr bool IsAsciiPunctuationFast(char c) {
  unsigned char uc = static_cast<unsigned char>(c);
  // ASCII punctuation ranges: 0x21-0x2F, 0x3A-0x40, 0x5B-0x60, 0x7B-0x7E
  return (uc >= 0x21 && uc <= 0x2F) || (uc >= 0x3A && uc <= 0x40) ||
         (uc >= 0x5B && uc <= 0x60) || (uc >= 0x7B && uc <= 0x7E);
}

// Fast ASCII path for Unicode whitespace - inlined constexpr
inline constexpr bool IsAsciiWhitespaceFast(char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f';
}

// Check if a Unicode code point is in P (punctuation) or S (symbol) categories
// This is used for CommonMark's definition of "Unicode punctuation character"
inline bool IsUnicodePunctuation(uint32_t cp) {
  // Fast ASCII path using constexpr inline check (covers >95% of cases)
  if (cp <= 0x7E) [[likely]] {
    return IsAsciiPunctuationFast(static_cast<char>(cp));
  }
  // Common Unicode punctuation and symbols (P and S categories)
  // This covers the most common cases; a full implementation would need
  // complete Unicode category tables

  // General Punctuation (U+2000-U+206F)
  if (cp >= 0x2000 && cp <= 0x206F) return true;
  // Supplemental Punctuation (U+2E00-U+2E7F)
  if (cp >= 0x2E00 && cp <= 0x2E7F) return true;
  // CJK Symbols and Punctuation (U+3000-U+303F)
  if (cp >= 0x3000 && cp <= 0x303F) return true;
  // Currency Symbols (U+20A0-U+20CF) - Sc category
  if (cp >= 0x20A0 && cp <= 0x20CF) return true;
  // Latin-1 punctuation and symbols
  if (cp >= 0x00A0 && cp <= 0x00BF) return true;  // Includes £ (00A3), ¥, etc.
  // Modifier symbols, letterlike symbols
  if (cp >= 0x02B0 && cp <= 0x02FF) return true;
  // Misc symbols (U+2600-U+26FF)
  if (cp >= 0x2600 && cp <= 0x26FF) return true;
  // Dingbats (U+2700-U+27BF)
  if (cp >= 0x2700 && cp <= 0x27BF) return true;
  // Math operators (U+2200-U+22FF)
  if (cp >= 0x2200 && cp <= 0x22FF) return true;
  // Arrows (U+2190-U+21FF)
  if (cp >= 0x2190 && cp <= 0x21FF) return true;
  // Box drawing (U+2500-U+257F)
  if (cp >= 0x2500 && cp <= 0x257F) return true;
  // Geometric shapes (U+25A0-U+25FF)
  if (cp >= 0x25A0 && cp <= 0x25FF) return true;
  // Misc Technical (U+2300-U+23FF)
  if (cp >= 0x2300 && cp <= 0x23FF) return true;
  // Halfwidth/Fullwidth punctuation (U+FF00-U+FFEF)
  if (cp >= 0xFF00 && cp <= 0xFFEF) return true;
  // Small form variants (U+FE50-U+FE6F)
  if (cp >= 0xFE50 && cp <= 0xFE6F) return true;
  // CJK compatibility (U+FE30-U+FE4F)
  if (cp >= 0xFE30 && cp <= 0xFE4F) return true;
  // Adlam Numbers (U+1E2FF is in this area - 𞋿)
  if (cp >= 0x1E2C0 && cp <= 0x1E2FF) return true;
  // Emoticons, emoji, etc. (various planes) - treat as symbols
  if (cp >= 0x1F300 && cp <= 0x1F9FF) return true;

  return false;
}

// Check if code point is Unicode whitespace (Zs category + ASCII whitespace)
inline bool IsUnicodeWhitespaceCodepoint(uint32_t cp) {
  // Fast ASCII path using constexpr inline check (covers >95% of cases)
  if (cp <= 0x7E) [[likely]] {
    return IsAsciiWhitespaceFast(static_cast<char>(cp));
  }
  // Unicode Zs (space separator) category
  if (cp == 0x00A0) return true;                  // NO-BREAK SPACE
  if (cp == 0x1680) return true;                  // OGHAM SPACE MARK
  if (cp >= 0x2000 && cp <= 0x200A) return true;  // Various spaces
  if (cp == 0x202F) return true;                  // NARROW NO-BREAK SPACE
  if (cp == 0x205F) return true;                  // MEDIUM MATHEMATICAL SPACE
  if (cp == 0x3000) return true;                  // IDEOGRAPHIC SPACE
  return false;
}

// Check if a character is Unicode whitespace (simplified)
// Uses lookup table for O(1) performance
inline bool IsUnicodeWhitespace(char c) {
  // Lookup table for ASCII whitespace: space(0x20), tab(0x09), newline(0x0A),
  // carriage return(0x0D), form feed(0x0C)
  static constexpr uint8_t kWhitespaceTable[256] = {
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      1,
      1,
      0,
      1,
      1,
      0,
      0,  // 0x00-0x0F:
          // tab,lf,ff,cr
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,  // 0x10-0x1F
      1,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,  // 0x20-0x2F: space
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,  // 0x30-0x3F
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,  // 0x40-0x7F
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,  // 0x80-0xFF
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
  };
  return kWhitespaceTable[static_cast<unsigned char>(c)] != 0;
}

// Check for Unicode whitespace at position in a string (handles multi-byte
// UTF-8) Returns number of bytes to skip if whitespace, 0 otherwise
inline size_t IsUnicodeWhitespaceAt(std::string_view s, size_t pos) {
  if (pos >= s.size()) return 0;
  unsigned char c = static_cast<unsigned char>(s[pos]);
  // Fast path for ASCII whitespace using existing lookup
  if (IsUnicodeWhitespace(static_cast<char>(c))) {
    return 1;
  }
  // UTF-8 NO-BREAK SPACE: 0xC2 0xA0
  if (c == 0xC2 && pos + 1 < s.size() &&
      static_cast<unsigned char>(s[pos + 1]) == 0xA0) {
    return 2;
  }
  return 0;
}

// Convert a Unicode code point to UTF-8
// Uses fixed buffer to avoid multiple string reallocations
inline std::pmr::string CodePointToUtf8(uint32_t cp) {
  char buf[4];
  size_t len;

  if (cp == 0 || cp >= 0x110000 || (cp >= 0xD800 && cp <= 0xDFFF))
      [[unlikely]] {
    // Null, out-of-range or surrogate (not encodable in valid UTF-8) code
    // point - use replacement character U+FFFD
    return "\xEF\xBF\xBD";
  } else if (cp < 0x80) {
    buf[0] = static_cast<char>(cp);
    len = 1;
  } else if (cp < 0x800) {
    buf[0] = static_cast<char>(0xC0 | (cp >> 6));
    buf[1] = static_cast<char>(0x80 | (cp & 0x3F));
    len = 2;
  } else if (cp < 0x10000) {
    buf[0] = static_cast<char>(0xE0 | (cp >> 12));
    buf[1] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    buf[2] = static_cast<char>(0x80 | (cp & 0x3F));
    len = 3;
  } else {
    buf[0] = static_cast<char>(0xF0 | (cp >> 18));
    buf[1] = static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
    buf[2] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    buf[3] = static_cast<char>(0x80 | (cp & 0x3F));
    len = 4;
  }
  return std::pmr::string(buf, len);
}

// Simple Unicode case-folding for a code point
// Returns the lowercase equivalent (or the same code point if no folding)
inline uint32_t UnicodeCaseFold(uint32_t cp) {
  // ASCII uppercase -> lowercase
  if (cp >= 'A' && cp <= 'Z') {
    return cp + 32;
  }
  // Latin-1 Supplement uppercase (U+00C0-U+00DE, except U+00D7 multiplication
  // sign)
  if (cp >= 0x00C0 && cp <= 0x00D6) {
    return cp + 32;  // À-Ö -> à-ö
  }
  if (cp >= 0x00D8 && cp <= 0x00DE) {
    return cp + 32;  // Ø-Þ -> ø-þ
  }
  // Latin Extended-A (pairs mostly 32 apart)
  if (cp >= 0x0100 && cp <= 0x012F) {
    if ((cp & 1) == 0) return cp + 1;  // Even code points are uppercase
  }
  if (cp >= 0x0132 && cp <= 0x0137) {
    if ((cp & 1) == 0) return cp + 1;
  }
  if (cp >= 0x0139 && cp <= 0x0148) {
    if ((cp & 1) == 1) return cp + 1;  // Odd code points are uppercase here
  }
  if (cp >= 0x014A && cp <= 0x0177) {
    if ((cp & 1) == 0) return cp + 1;
  }
  if (cp == 0x0178) return 0x00FF;  // Ÿ -> ÿ
  if (cp >= 0x0179 && cp <= 0x017E) {
    if ((cp & 1) == 1) return cp + 1;
  }
  // Greek uppercase (U+0391-U+03A9) -> lowercase (U+03B1-U+03C9)
  if (cp >= 0x0391 && cp <= 0x03A1) {
    return cp + 32;  // Α-Ρ -> α-ρ
  }
  if (cp >= 0x03A3 && cp <= 0x03A9) {
    return cp + 32;  // Σ-Ω -> σ-ω
  }
  // Cyrillic uppercase (U+0410-U+042F) -> lowercase (U+0430-U+044F)
  if (cp >= 0x0410 && cp <= 0x042F) {
    return cp + 32;
  }
  // More Cyrillic (U+0400-U+040F) -> (U+0450-U+045F)
  if (cp >= 0x0400 && cp <= 0x040F) {
    return cp + 80;
  }
  // German capital sharp S (ẞ U+1E9E) is handled specially in
  // NormalizeLinkLabel because it folds to "ss" (two characters) in full case
  // folding Latin Extended Additional
  if (cp >= 0x1E00 && cp <= 0x1E95) {
    if ((cp & 1) == 0) return cp + 1;
  }
  // Latin Extended-B (various)
  if (cp >= 0x01A0 && cp <= 0x01A5) {
    if ((cp & 1) == 0) return cp + 1;
  }
  if (cp >= 0x01DE && cp <= 0x01EF) {
    if ((cp & 1) == 0) return cp + 1;
  }
  // Greek Extended
  if (cp >= 0x1F08 && cp <= 0x1F0F) return cp - 8;
  if (cp >= 0x1F18 && cp <= 0x1F1D) return cp - 8;
  if (cp >= 0x1F28 && cp <= 0x1F2F) return cp - 8;
  if (cp >= 0x1F38 && cp <= 0x1F3F) return cp - 8;
  if (cp >= 0x1F48 && cp <= 0x1F4D) return cp - 8;
  if (cp >= 0x1F68 && cp <= 0x1F6F) return cp - 8;

  return cp;  // No case folding
}

// Normalize a link label (case-fold and collapse whitespace)
// Uses Unicode-aware case folding for CommonMark compliance
// C++20: optimized with fast path for ASCII-only labels using ranges::any_of
// Writes the normalized label into `result` (replacing its contents), so
// callers can reuse one buffer across lookups.
inline void NormalizeLinkLabelInto(std::string_view label,
                                   std::pmr::string& result) {
  // Fast path: check if label is ASCII-only (common case) using ranges::any_of
  bool all_ascii =
      !std::ranges::any_of(label, [](unsigned char c) { return c >= 0x80; });

  result.clear();

  if (all_ascii) {
    // Fast ASCII-only path: simple lowercase + whitespace collapse, written
    // straight into a buffer sized for the worst case (no collapsing).
    result.resize(label.size());
    char* out = result.data();
    size_t n = 0;
    bool in_whitespace = false;
    bool first = true;
    for (char c : label) {
      if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f') {
        if (!first && !in_whitespace) {
          out[n++] = ' ';
          in_whitespace = true;
        }
      } else {
        out[n++] = static_cast<char>(CaseFold(static_cast<uint8_t>(c)));
        in_whitespace = false;
        first = false;
      }
    }
    result.resize(n);
  } else {
    result.reserve(label.size());
    // Slow path: full Unicode handling
    bool in_whitespace = false;
    bool first = true;
    size_t i = 0;

    while (i < label.size()) {
      // Check for whitespace (including multi-byte Unicode whitespace)
      size_t ws_len = IsUnicodeWhitespaceAt(label, i);
      if (ws_len > 0) {
        if (!first && !in_whitespace) {
          result += ' ';
          in_whitespace = true;
        }
        i += ws_len;
        continue;
      }

      // Decode UTF-8 code point
      auto [cp, len] = DecodeUtf8At(label, i);
      if (len == 0) {
        ++i;
        continue;
      }

      // Apply case folding - special handling for characters that fold to
      // multiple chars
      if (cp == 0x1E9E) {
        // German capital sharp S (ẞ) folds to "ss" in full case folding
        result += "ss";
      } else {
        uint32_t folded = UnicodeCaseFold(cp);
        // Encode back to UTF-8
        result += CodePointToUtf8(folded);
      }

      in_whitespace = false;
      first = false;
      i += len;
    }
  }

  // Trim trailing space
  if (!result.empty() && result.back() == ' ') {
    result.pop_back();
  }
}

// True when NormalizeLinkLabel(label) == label, i.e. the label is ASCII with
// no uppercase letters, no whitespace other than single interior spaces.
// Lets lookups use the label as-is (the common case) without copying it.
inline bool IsNormalizedLinkLabel(std::string_view label) {
  if (label.empty()) return true;
  if (label.front() == ' ' || label.back() == ' ') return false;
  char prev = 0;
  for (char c : label) {
    unsigned char u = static_cast<unsigned char>(c);
    if (u >= 0x80 || (u >= 'A' && u <= 'Z') || c == '\t' || c == '\n' ||
        c == '\r' || c == '\f' || (c == ' ' && prev == ' ')) {
      return false;
    }
    prev = c;
  }
  return true;
}

inline std::pmr::string NormalizeLinkLabel(std::string_view label) {
  std::pmr::string result;
  NormalizeLinkLabelInto(label, result);
  return result;
}

// HTML entity escaping - optimized with lookup table and batch copying
// For in-place rendering (no temp allocation), use EscapeHtmlTo instead
inline std::pmr::string EscapeHtml(std::string_view text) {
  std::pmr::string result;
  result.reserve(text.size() + text.size() / 8);
  EscapeHtmlTo(text, result);
  return result;
}

// URL encoding for link destinations - optimized with in-place variant
// Uses lookup table for O(1) character classification and precomputed hex table
inline std::pmr::string EncodeUrl(std::string_view url) {
  std::pmr::string result;
  result.reserve(url.size() + url.size() / 4);
  EncodeUrlTo(url, result);
  return result;
}

// Trim leading and trailing whitespace
inline std::string_view Trim(std::string_view s) {
  size_t start = 0;
  while (start < s.size() && IsUnicodeWhitespace(s[start])) {
    ++start;
  }
  size_t end = s.size();
  while (end > start && IsUnicodeWhitespace(s[end - 1])) {
    --end;
  }
  return s.substr(start, end - start);
}

// Trim only leading whitespace
inline std::string_view TrimLeft(std::string_view s) {
  size_t start = 0;
  while (start < s.size() && IsUnicodeWhitespace(s[start])) {
    ++start;
  }
  return s.substr(start);
}

// Case-insensitive prefix check without temporary string allocations
inline bool StartsWithInsensitive(std::string_view str,
                                  std::string_view prefix) {
  if (str.size() < prefix.size()) return false;
  for (size_t i = 0; i < prefix.size(); ++i) {
    if (detail::CaseFold(static_cast<unsigned char>(str[i])) !=
        detail::CaseFold(static_cast<unsigned char>(prefix[i]))) {
      return false;
    }
  }
  return true;
}

// Case-insensitive substring search without temporary string allocations
inline bool StringContainsInsensitive(std::string_view str,
                                      std::string_view substr) {
  if (substr.empty() || str.size() < substr.size()) return false;
  // Fast path: find first character of substr in str
  char first = static_cast<char>(
      detail::CaseFold(static_cast<unsigned char>(substr[0])));
  for (size_t i = 0; i <= str.size() - substr.size(); ++i) {
    if (static_cast<char>(
            detail::CaseFold(static_cast<unsigned char>(str[i]))) != first)
      continue;
    bool match = true;
    for (size_t j = 1; j < substr.size(); ++j) {
      if (static_cast<char>(
              detail::CaseFold(static_cast<unsigned char>(str[i + j]))) !=
          static_cast<char>(
              detail::CaseFold(static_cast<unsigned char>(substr[j])))) {
        match = false;
        break;
      }
    }
    if (match) return true;
  }
  return false;
}

// Case-insensitive substring search returning the index of the first
// occurrence of `needle` in `hay` (or npos).
inline size_t FindInsensitive(std::string_view hay, std::string_view needle) {
  if (needle.empty() || hay.size() < needle.size())
    return std::string_view::npos;
  for (size_t i = 0; i + needle.size() <= hay.size(); ++i) {
    if (StartsWithInsensitive(hay.substr(i, needle.size()), needle)) return i;
  }
  return std::string_view::npos;
}

// Parse the attributes of an opening HTML tag, e.g.
// `<details data-tool="true" class='x' hidden>` yields
// (data-tool, "true"), (class, "x"), (hidden, ""). Pairs are kept in document
// order; boolean attributes have an empty value. Malformed input (a value
// without a name, an unterminated quote) simply stops the parse.
inline std::pmr::vector<std::pair<std::pmr::string, std::pmr::string>>
ParseTagAttributes(std::string_view tag) {
  std::pmr::vector<std::pair<std::pmr::string, std::pmr::string>> attrs;
  auto is_space = [](char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; };
  size_t pos = 0;
  // Skip the leading '<' and the tag name.
  while (pos < tag.size() && !is_space(tag[pos]) && tag[pos] != '>') ++pos;
  while (pos < tag.size()) {
    while (pos < tag.size() && is_space(tag[pos])) ++pos;
    if (pos >= tag.size() || tag[pos] == '>') break;
    size_t name_start = pos;
    while (pos < tag.size() && tag[pos] != '=' && tag[pos] != '>'
           && !is_space(tag[pos]))
      ++pos;
    std::string_view name = tag.substr(name_start, pos - name_start);
    if (name.empty()) break;  // Malformed: no attribute name.
    std::string_view value;
    if (pos < tag.size() && tag[pos] == '=') {
      ++pos;
      if (pos < tag.size() && (tag[pos] == '"' || tag[pos] == '\'')) {
        char quote = tag[pos];
        ++pos;
        size_t value_start = pos;
        while (pos < tag.size() && tag[pos] != quote) ++pos;
        value = tag.substr(value_start, pos - value_start);
        if (pos < tag.size()) ++pos;  // Skip the closing quote.
      } else {
        size_t value_start = pos;
        while (pos < tag.size() && !is_space(tag[pos]) && tag[pos] != '>')
          ++pos;
        value = tag.substr(value_start, pos - value_start);
      }
    }
    attrs.emplace_back(std::pmr::string(name), std::pmr::string(value));
  }
  return attrs;
}

// Count leading spaces (tabs count as 4 spaces to next tab stop)
inline int CountIndent(std::string_view line, int* consumed_chars = nullptr) {
  int indent = 0;
  size_t i = 0;
  for (; i < line.size(); ++i) {
    if (line[i] == ' ') {
      ++indent;
    } else if (line[i] == '\t') {
      indent = (indent / 4 + 1) * 4;  // Advance to next tab stop
    } else {
      break;
    }
  }
  if (consumed_chars) {
    *consumed_chars = static_cast<int>(i);
  }
  return indent;
}

// Remove N spaces of indentation (handling tabs)
// Optimized with fast path for common case of n spaces at start
// Appends `line` with up to `n` columns of leading whitespace removed into
// `out`. Appending directly lets callers avoid materializing an intermediate
// string when the result is immediately consumed.
inline void AppendRemoveIndent(std::string_view line, int n,
                               std::pmr::string& out) {
  size_t i = 0;
  int removed = 0;

  while (n > 0 && i < line.size() && removed < n) {
    if (line[i] == ' ') {
      ++removed;
      ++i;
    } else if (line[i] == '\t') {
      int tab_width = 4 - (removed % 4);
      if (removed + tab_width <= n) {
        removed += tab_width;
        ++i;
      } else {
        // Partial tab - add spaces for the portion of the tab we didn't use
        int remaining = n - removed;
        removed = n;
        ++i;
        int leftover = tab_width - remaining;
        out.append(leftover, ' ');
      }
    } else {
      break;
    }
  }

  out.append(line.data() + i, line.size() - i);
}

inline std::pmr::string RemoveIndent(std::string_view line, int n) {
  std::pmr::string result;
  AppendRemoveIndent(line, n, result);
  return result;
}

// Append blockquote content (after > and optional space) directly into out,
// with proper tab expansion. Returns true if the line had a > prefix, false
// otherwise (and out is untouched). Fast path: bulk append when no tabs.
inline bool AppendBlockQuoteContent(std::string_view line,
                                    std::pmr::string& out) {
  size_t i = 0;
  int column = 0;

  // Skip leading whitespace (up to 3 spaces)
  while (i < line.size() && column < 3) {
    if (line[i] == ' ') {
      ++column;
      ++i;
    } else if (line[i] == '\t') {
      int next_col = (column / 4 + 1) * 4;
      if (next_col <= 3) {
        column = next_col;
        ++i;
      } else {
        break;
      }
    } else {
      break;
    }
  }

  // Check for >
  if (i >= line.size() || line[i] != '>') {
    return false;
  }
  ++i;
  ++column;

  // Handle optional space after > (can be partial tab)
  int content_start_column = column;
  if (i < line.size()) {
    if (line[i] == ' ') {
      ++i;
      content_start_column = column + 1;
    } else if (line[i] == '\t') {
      // Tab after > - consume part of the tab as the "optional space"
      int tab_end = (column / 4 + 1) * 4;
      int spaces_from_tab = tab_end - column;
      ++i;
      content_start_column = tab_end;
      // Append remaining (spaces_from_tab - 1) leading spaces
      for (int j = 0; j < spaces_from_tab - 1; ++j) {
        out += ' ';
      }
      // Expand remaining tabs in the content
      int col = content_start_column;
      while (i < line.size()) {
        if (line[i] == '\t') {
          int next_col = (col / 4 + 1) * 4;
          for (int k = 0; k < next_col - col; ++k) {
            out += ' ';
          }
          col = next_col;
        } else {
          out += line[i];
          ++col;
        }
        ++i;
      }
      return true;
    }
  }

  // Fast path: no tabs — bulk append the rest
  if (line.find('\t', i) == std::string_view::npos) {
    out.append(line.data() + i, line.size() - i);
    return true;
  }

  // Slow path: expand tabs in content
  int col = content_start_column;
  while (i < line.size()) {
    if (line[i] == '\t') {
      int next_col = (col / 4 + 1) * 4;
      for (int k = 0; k < next_col - col; ++k) {
        out += ' ';
      }
      col = next_col;
    } else {
      out += line[i];
      ++col;
    }
    ++i;
  }
  return true;
}

// Expand tabs to spaces (tab stops every 4 columns)
// Optimized with fast path and batch copying
inline std::pmr::string ExpandTabs(std::string_view line) {
  // Fast path: no tabs present
  size_t first_tab = line.find('\t');
  if (first_tab == std::string_view::npos) {
    return std::pmr::string(line);  // No tabs, zero-copy
  }

  std::pmr::string result;
  result.reserve(line.size() + 16);  // Extra space for tab expansion

  // Copy everything before first tab
  result.append(line.data(), first_tab);
  int column = static_cast<int>(first_tab);

  for (size_t i = first_tab; i < line.size(); ++i) {
    char c = line[i];
    if (c == '\t') {
      int next_col = (column / 4 + 1) * 4;
      int spaces = next_col - column;
      result.append(spaces, ' ');
      column = next_col;
    } else {
      result += c;
      ++column;
    }
  }
  return result;
}

// Check if line is blank (only whitespace)
// Uses lookup table for O(1) per-character checks
inline bool IsBlankLine(std::string_view line) {
  // Lookup table: 1 = blank character (space, tab, \r, \n), 0 = non-blank
  static constexpr uint8_t kBlankTable[256] = {
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      1,
      1,
      0,
      0,
      1,
      0,
      0,  // 0x00-0x0F: tab,lf,cr
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,  // 0x10-0x1F
      1,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,  // 0x20-0x2F: space
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,  // 0x30-0xFF (all
          // non-blank)
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
  };
  // Use efficient helper for bulk processing
  if (line.size() >= 8) {
    return IsSpanBlank(line.data(), line.size());
  }
  // Scalar fallback for short lines
  for (char c : line) {
    if (!kBlankTable[static_cast<unsigned char>(c)]) {
      return false;
    }
  }
  return true;
}

// =============================================================================
// GFM table (extension) helpers
// =============================================================================

// Remove a backslash that immediately precedes a pipe, matching cmark-gfm's
// unescape_pipes so that `\|` inside a cell becomes a literal `|`.
inline void UnescapePipes(std::pmr::string& s) {
  size_t r = 0, w = 0;
  while (r < s.size()) {
    if (s[r] == '\\' && r + 1 < s.size() && s[r + 1] == '|') {
      ++r;  // drop the backslash
    }
    s[w++] = s[r];
    ++r;
  }
  s.resize(w);
}

// True if `line` is a setext heading underline: a run of `=` or `-`, then
// optional whitespace, indented at most 3 spaces. Used so that `foo` + `---`
// stays a heading rather than a single-column table.
inline bool IsSetextUnderline(std::string_view line) {
  if (CountIndent(line) >= 4) return false;
  std::string_view t = TrimLeft(line);
  if (t.empty()) return false;
  char c = t[0];
  if (c != '=' && c != '-') return false;
  size_t i = 0;
  while (i < t.size() && t[i] == c) ++i;
  for (; i < t.size(); ++i) {
    if (t[i] != ' ' && t[i] != '\t') return false;
  }
  return true;
}

// True if a (trimmed) delimiter cell matches GFM's `:?-+:?`; fills alignment.
inline bool ClassifyDelimiterCell(std::string_view cell, TableAlign& align) {
  if (cell.empty()) return false;
  size_t i = 0;
  bool left = false, right = false;
  if (cell[i] == ':') {
    left = true;
    ++i;
  }
  size_t first_hyphen = i;
  while (i < cell.size() && cell[i] == '-') ++i;
  if (i == first_hyphen) return false;  // needs at least one hyphen
  if (i < cell.size() && cell[i] == ':') {
    right = true;
    ++i;
  }
  if (i != cell.size()) return false;  // trailing junk
  align = (left && right) ? TableAlign::kCenter
          : (left)        ? TableAlign::kLeft
          : (right)       ? TableAlign::kRight
                          : TableAlign::kNone;
  return true;
}

// Split a single GFM table row line into its cell contents (pipes removed,
// `\|` escapes resolved, cells trimmed). Mirrors cmark-gfm's row_from_string
// for a line that is exactly one row. Returns an empty vector if the line does
// not cleanly form a row of one or more cells.
inline std::pmr::vector<std::pmr::string> SplitTableRow(std::string_view line) {
  std::pmr::vector<std::pmr::string> cells;
  const size_t len = line.size();
  size_t offset = 0;

  auto is_space = [](char c) {
    return c == ' ' || c == '\t' || c == '\v' || c == '\f';
  };
  auto is_escaped = [](std::string_view s, size_t i) {
    if (s[i] != '\\' || i + 1 >= s.size()) return false;
    unsigned char c = s[i + 1];
    return c >= 0x21 && c <= 0x7e;  // backslash + ASCII punctuation
  };

  // Scan past the optional leading pipe (and following spaces).
  if (offset < len && line[offset] == '|') {
    ++offset;
    while (offset < len && is_space(line[offset])) ++offset;
  }

  bool expect_more = true;
  while (offset < len && expect_more) {
    // table_cell: (escaped_char | [^|\r\n])+
    const size_t cell_start = offset;
    size_t cell_matched = 0;
    while (offset < len) {
      if (is_escaped(line, offset)) {
        offset += 2;
        cell_matched += 2;
      } else if (line[offset] != '|' && line[offset] != '\r' &&
                 line[offset] != '\n') {
        ++offset;
        ++cell_matched;
      } else {
        break;
      }
    }
    // table_cell_end: [|] spacechar*
    size_t pipe_matched = 0;
    if (offset < len && line[offset] == '|') {
      ++offset;
      ++pipe_matched;
      while (offset < len && is_space(line[offset])) {
        ++offset;
        ++pipe_matched;
      }
    }

    if (cell_matched > 0 || pipe_matched > 0) {
      std::pmr::string cell(line.substr(cell_start, cell_matched));
      UnescapePipes(cell);
      // Trim ASCII whitespace (spaces, tabs) like cmark_strbuf_trim.
      size_t a = 0, b = cell.size();
      while (a < b && (cell[a] == ' ' || cell[a] == '\t')) ++a;
      while (b > a && (cell[b - 1] == ' ' || cell[b - 1] == '\t')) --b;
      cell = std::pmr::string(cell.substr(a, b - a));
      cells.push_back(std::move(cell));
    }

    if (pipe_matched > 0) {
      expect_more = true;
    } else {
      // table_row_end: trailing spaces only (a single line has no newline).
      while (offset < len && is_space(line[offset])) ++offset;
      if (offset != len) return {};
      expect_more = false;
    }
  }

  if (offset != len || cells.empty()) return {};
  return cells;
}

// True if `line` continues a GFM table as a body row: non-blank and not the
// start of a new block-level construct. Setext underlines do not apply here
// (a table is not a paragraph), so `===` is a row but `---` is a thematic
// break.
inline bool IsTableRowLine(std::string_view line) {
  if (IsBlankLine(line)) return false;
  if (CountIndent(line) >= 4) return false;  // indented code block
  std::string_view t = TrimLeft(line);
  if (t.empty()) return false;

  // ATX heading
  if (t[0] == '#') {
    size_t h = 0;
    while (h < t.size() && t[h] == '#') ++h;
    if (h <= 6 && (h >= t.size() || t[h] == ' ' || t[h] == '\t')) return false;
  }
  // Block quote
  if (t[0] == '>') return false;
  // Fenced code block
  if (t.size() >= 3 && (t[0] == '`' || t[0] == '~')) {
    char f = t[0];
    size_t fl = 0;
    while (fl < t.size() && t[fl] == f) ++fl;
    if (fl >= 3) {
      if (f == '~') return false;
      std::string_view info = Trim(t.substr(fl));
      if (info.find('`') == std::string_view::npos) return false;
    }
  }
  // Thematic break
  if (t.size() >= 3 && (t[0] == '-' || t[0] == '*' || t[0] == '_')) {
    char c = t[0];
    int count = 0;
    bool ok = true;
    for (char ch : t) {
      if (ch == c)
        ++count;
      else if (ch != ' ' && ch != '\t') {
        ok = false;
        break;
      }
    }
    if (ok && count >= 3) return false;
  }
  // List item
  if (t.starts_with("- ") || t.starts_with("+ ") || t.starts_with("* "))
    return false;
  if (detail::IsDigit(static_cast<unsigned char>(t[0]))) {
    size_t num_end = 0;
    while (num_end < t.size() &&
           detail::IsDigit(static_cast<unsigned char>(t[num_end])))
      ++num_end;
    if (num_end + 1 < t.size() && (t[num_end] == '.' || t[num_end] == ')') &&
        (t[num_end + 1] == ' ' || t[num_end + 1] == '\t')) {
      return false;
    }
  }
  // HTML block (types 1-6 can terminate a table)
  if (t.starts_with("<")) {
    static constexpr std::array type1_tags = {
        std::string_view("<script"), std::string_view("<pre"),
        std::string_view("<style"), std::string_view("<textarea")};
    for (auto tag : type1_tags) {
      if (StartsWithInsensitive(t, tag) &&
          (t.size() == tag.size() || t[tag.size()] == ' ' ||
           t[tag.size()] == '>' || t[tag.size()] == '\t')) {
        return false;
      }
    }
    if (t.starts_with("<!--") || t.starts_with("<?") ||
        t.starts_with("<![CDATA[")) {
      return false;
    }
    if (StartsWithInsensitive(t, "<!doctype")) return false;
    static constexpr std::array type6_tags = {
        std::string_view("address"),  std::string_view("article"),
        std::string_view("aside"),    std::string_view("base"),
        std::string_view("basefont"), std::string_view("blockquote"),
        std::string_view("body"),     std::string_view("caption"),
        std::string_view("center"),   std::string_view("col"),
        std::string_view("colgroup"), std::string_view("dd"),
        std::string_view("details"),  std::string_view("dialog"),
        std::string_view("dir"),      std::string_view("div"),
        std::string_view("dl"),       std::string_view("dt"),
        std::string_view("fieldset"), std::string_view("figcaption"),
        std::string_view("figure"),   std::string_view("footer"),
        std::string_view("form"),     std::string_view("frame"),
        std::string_view("frameset"), std::string_view("h1"),
        std::string_view("h2"),       std::string_view("h3"),
        std::string_view("h4"),       std::string_view("h5"),
        std::string_view("h6"),       std::string_view("head"),
        std::string_view("header"),   std::string_view("hr"),
        std::string_view("html"),     std::string_view("iframe"),
        std::string_view("legend"),   std::string_view("li"),
        std::string_view("link"),     std::string_view("main"),
        std::string_view("menu"),     std::string_view("menuitem"),
        std::string_view("nav"),      std::string_view("noframes"),
        std::string_view("ol"),       std::string_view("optgroup"),
        std::string_view("option"),   std::string_view("p"),
        std::string_view("param"),    std::string_view("search"),
        std::string_view("section"),  std::string_view("summary"),
        std::string_view("table"),    std::string_view("tbody"),
        std::string_view("td"),       std::string_view("tfoot"),
        std::string_view("th"),       std::string_view("thead"),
        std::string_view("title"),    std::string_view("tr"),
        std::string_view("track"),    std::string_view("ul")};
    bool is_closing = (t.size() >= 2 && t[1] == '/');
    size_t tag_start = is_closing ? 2 : 1;
    size_t tag_end = tag_start;
    while (tag_end < t.size() &&
           (detail::IsAlnum(static_cast<unsigned char>(t[tag_end])) ||
            t[tag_end] == '-')) {
      ++tag_end;
    }
    if (tag_end > tag_start) {
      std::string_view tag_name = t.substr(tag_start, tag_end - tag_start);
      for (auto name : type6_tags) {
        if (StartsWithInsensitive(tag_name, name) &&
            tag_name.size() == name.size()) {
          if (tag_end >= t.size() || t[tag_end] == ' ' || t[tag_end] == '>' ||
              t[tag_end] == '\t' || t[tag_end] == '/') {
            return false;
          }
        }
      }
    }
  }
  return true;
}

// =============================================================================
// Inline Parser Special Character Lookup Table (cmark-style optimization)
// Marks characters that require special handling in inline parsing
// This enables bulk scanning of "safe" text runs in the hot path
// =============================================================================

inline constexpr auto MakeInlineSpecialTable() {
  std::array<uint8_t, 256> table{};
  // Characters that trigger special inline parsing logic:
  // backslash (escape), ampersand (entity), backtick (code span),
  // less-than (autolink/HTML), asterisk/underscore (emphasis),
  // brackets (links), exclamation (image), newline (soft/hard break).
  // Space is deliberately not special (it would stop the scan at every
  // word); a hard break's trailing spaces are found from the newline.
  table['\\'] = 1;  // Escape character
  table['&'] = 1;   // Entity reference
  table['`'] = 1;   // Code span
  table['<'] = 1;   // Autolink / HTML inline
  table['*'] = 1;   // Emphasis / strong
  table['_'] = 1;   // Emphasis / strong
  table['~'] = 1;   // GFM strikethrough (only when the extension is enabled)
  table['$'] = 1;   // LaTeX math (only when the extension is enabled)
  table['['] = 1;   // Link open
  table[']'] = 1;   // Link close
  table['!'] = 1;   // Image prefix
  table['\n'] = 1;  // Soft break
  return table;
}

inline constexpr auto kInlineSpecialTable = MakeInlineSpecialTable();

// Fast path: find next special character position using lookup table
// Scans ahead past runs of "safe" (non-special) characters in bulk
// C++20: 8-wide unrolled scan for significantly better throughput
inline size_t FindNextSpecialChar(std::string_view text, size_t start) {
  size_t i = start;
  const size_t len = text.size();

  // 8-wide unrolled scan - handles >95% of characters in typical text
  while (i + 7 < len) {
    uint8_t e0 = kInlineSpecialTable[static_cast<unsigned char>(text[i])];
    uint8_t e1 = kInlineSpecialTable[static_cast<unsigned char>(text[i + 1])];
    uint8_t e2 = kInlineSpecialTable[static_cast<unsigned char>(text[i + 2])];
    uint8_t e3 = kInlineSpecialTable[static_cast<unsigned char>(text[i + 3])];
    if (e0 || e1 || e2 || e3) {
      if (e0) return i;
      if (e1) return i + 1;
      if (e2) return i + 2;
      return i + 3;
    }
    uint8_t e4 = kInlineSpecialTable[static_cast<unsigned char>(text[i + 4])];
    uint8_t e5 = kInlineSpecialTable[static_cast<unsigned char>(text[i + 5])];
    uint8_t e6 = kInlineSpecialTable[static_cast<unsigned char>(text[i + 6])];
    uint8_t e7 = kInlineSpecialTable[static_cast<unsigned char>(text[i + 7])];
    if (e4 || e5 || e6 || e7) {
      if (e4) return i + 4;
      if (e5) return i + 5;
      if (e6) return i + 6;
      return i + 7;
    }
    i += 8;
  }

  // Scalar fallback for remaining characters
  while (i < len) {
    if (kInlineSpecialTable[static_cast<unsigned char>(text[i])]) return i;
    ++i;
  }
  return i;
}

// =============================================================================
// LineBuffer - Cache-friendly line storage
// =============================================================================
// Instead of vector<string> (many allocations, poor cache locality),
// stores line offsets into a contiguous buffer. Only copies data when
// null characters need replacement.

class LineBuffer {
 public:
  LineBuffer() = default;

  // Empty buffer whose storage comes from `resource`.
  explicit LineBuffer(std::pmr::memory_resource* resource)
      : line_offsets_(resource), buffer_(resource) {}

  explicit LineBuffer(std::string_view input, bool no_nulls = false) {
    Reset(input, no_nulls);
  }

  // Not copyable. Moves re-point data_ptr_ when it refers to buffer_, whose
  // characters may live in the object itself (SSO) and so move with it.
  LineBuffer(const LineBuffer&) = delete;
  LineBuffer& operator=(const LineBuffer&) = delete;
  LineBuffer(LineBuffer&& other) { *this = std::move(other); }
  LineBuffer& operator=(LineBuffer&& other) {
    if (this != &other) {
      bool owns = other.data_ptr_ != nullptr &&
                  other.data_ptr_ == other.buffer_.data();
      line_offsets_ = std::move(other.line_offsets_);
      buffer_ = std::move(other.buffer_);
      data_ptr_ = owns ? buffer_.data() : other.data_ptr_;
      other.data_ptr_ = nullptr;
    }
    return *this;
  }

  // Split `input` into lines, replacing any previous contents.
  void Reset(std::string_view input, bool no_nulls = false) {
    line_offsets_.clear();
    buffer_.clear();
    data_ptr_ = nullptr;
    if (input.empty()) [[unlikely]] {
      return;
    }

    // Single pass: split on line terminators, scanning 8 bytes at a time.
    // A NUL (which must be replaced) sends us to the copying slow path;
    // `no_nulls` marks content we built ourselves (guaranteed NUL-free, e.g.
    // nested blockquote/list buffers), for which NULs need no handling.
    const char* data = input.data();
    const size_t len = input.size();
    line_offsets_.reserve(len / 32 + 4);
    size_t line_start = 0;
    size_t i = 0;
    while ((i = FindLineBreakOrNul(data, i, len)) < len) {
      char c = data[i];
      if (c == '\0') [[unlikely]] {
        if (!no_nulls) {
          BuildReplacingNulls(input);
          return;
        }
        ++i;
        continue;
      }
      line_offsets_.emplace_back(line_start, i - line_start);
      if (c == '\r' && i + 1 < len && data[i + 1] == '\n') ++i;
      line_start = ++i;
    }
    // Add final line
    if (len > line_start || (input.back() == '\n' || input.back() == '\r')) {
      line_offsets_.emplace_back(line_start, len - line_start);
    }
    data_ptr_ = data;
  }

  size_t size() const { return line_offsets_.size(); }
  bool empty() const { return line_offsets_.empty(); }

  std::string_view operator[](size_t idx) const {
    const auto& [offset, len] = line_offsets_[idx];
    return std::string_view(data_ptr_ + offset, len);
  }

  // Byte offset (into the underlying data buffer) where line `idx` begins.
  size_t OffsetOf(size_t idx) const { return line_offsets_[idx].offset; }

 private:
  // Index of the first '\n', '\r' or NUL in data[i, len), or len. Checks a
  // word at a time with the classic SWAR "has zero byte" test, falling back
  // to a byte loop to locate the exact match (endian-independent).
  static size_t FindLineBreakOrNul(const char* data, size_t i, size_t len) {
    constexpr uint64_t kOnes = 0x0101010101010101ULL;
    constexpr uint64_t kHighs = 0x8080808080808080ULL;
    auto has_zero = [](uint64_t v) { return (v - kOnes) & ~v & kHighs; };
    for (; i + 8 <= len; i += 8) {
      uint64_t word;
      std::memcpy(&word, data + i, sizeof(word));
      if (has_zero(word) | has_zero(word ^ (kOnes * '\n')) |
          has_zero(word ^ (kOnes * '\r'))) {
        break;
      }
    }
    for (; i < len; ++i) {
      char c = data[i];
      if (c == '\n' || c == '\r' || c == '\0') return i;
    }
    return len;
  }

  // Slow path: copy the input, replacing each NUL with U+FFFD.
  void BuildReplacingNulls(std::string_view input) {
    line_offsets_.clear();
    buffer_.reserve(input.size() + 16);
    size_t line_start = 0;
    for (size_t i = 0; i < input.size(); ++i) {
      char c = input[i];
      if (c == '\n') {
        line_offsets_.emplace_back(line_start, buffer_.size() - line_start);
        line_start = buffer_.size();
      } else if (c == '\r') {
        line_offsets_.emplace_back(line_start, buffer_.size() - line_start);
        if (i + 1 < input.size() && input[i + 1] == '\n') ++i;
        line_start = buffer_.size();
      } else if (c == '\0') {
        buffer_ += "\xEF\xBF\xBD";  // UTF-8 replacement character
      } else {
        buffer_ += c;
      }
    }
    // Add final line
    if (buffer_.size() > line_start ||
        (input.back() == '\n' || input.back() == '\r')) {
      line_offsets_.emplace_back(line_start, buffer_.size() - line_start);
    }
    data_ptr_ = buffer_.data();
  }

  struct LineOffset {
    size_t offset;
    size_t length;
  };
  std::pmr::vector<LineOffset> line_offsets_;  // Contiguous array of offsets
  std::pmr::string buffer_;                    // Only used if nulls present
  const char* data_ptr_ = nullptr;  // Points to buffer_ or original input
};

// Legacy SplitLines for compatibility (used by some functions)
inline std::pmr::vector<std::pmr::string> SplitLines(std::string_view input) {
  LineBuffer buf(input);
  std::pmr::vector<std::pmr::string> lines;
  lines.reserve(buf.size());
  for (size_t i = 0; i < buf.size(); ++i) {
    lines.emplace_back(buf[i]);
  }
  return lines;
}

// Helper for transparent string_view lookup in unordered_map
struct StringHash {
  using is_transparent = void;
  size_t operator()(std::string_view sv) const noexcept {
    return std::hash<std::string_view>{}(sv);
  }
  size_t operator()(const std::pmr::string& s) const noexcept {
    return std::hash<std::string_view>{}(s);
  }
};

struct StringEqual {
  using is_transparent = void;
  bool operator()(std::string_view lhs, std::string_view rhs) const noexcept {
    return lhs == rhs;
  }
};

// HTML named entity map (common entities from CommonMark spec)
// Uses transparent lookup to avoid string_view->string conversion
inline std::string_view LookupHtmlEntity(std::string_view name) {
  // This is a subset - full CommonMark compliance requires all HTML5 entities
  // Sorted array + binary search (md4c pattern): avoids unordered_map
  // allocation overhead and provides better cache locality for the small entity
  // table
  struct EntityEntry {
    const char* name;
    std::string_view value;
  };

  static constexpr EntityEntry kEntities[] = {
      {"AElig", "\xC3\x86"},
      {"Auml", "\xC3\x84"},
      {"ClockwiseContourIntegral", "\xE2\x88\xB2"},
      {"Dagger", "\xE2\x80\xA1"},
      {"Dcaron", "\xC4\x8E"},
      {"DifferentialD", "\xE2\x85\x86"},
      {"HilbertSpace", "\xE2\x84\x8B"},
      {"Ouml", "\xC3\x96"},
      {"Uuml", "\xC3\x9C"},
      {"amp", "&"},
      {"apos", "'"},
      {"auml", "\xC3\xA4"},
      {"bull", "\xE2\x80\xA2"},
      {"cent", "\xC2\xA2"},
      {"clubs", "\xE2\x99\xA3"},
      {"copy", "\xC2\xA9"},
      {"dagger", "\xE2\x80\xA0"},
      {"darr", "\xE2\x86\x93"},
      {"deg", "\xC2\xB0"},
      {"diams", "\xE2\x99\xA6"},
      {"divide", "\xC3\xB7"},
      {"euro", "\xE2\x82\xAC"},
      {"frac12", "\xC2\xBD"},
      {"frac14", "\xC2\xBC"},
      {"frac34", "\xC2\xBE"},
      {"gt", ">"},
      {"harr", "\xE2\x86\x94"},
      {"hearts", "\xE2\x99\xA5"},
      {"hellip", "\xE2\x80\xA6"},
      {"laquo", "\xC2\xAB"},
      {"larr", "\xE2\x86\x90"},
      {"ldquo", "\xE2\x80\x9C"},
      {"lsquo", "\xE2\x80\x98"},
      {"lt", "<"},
      {"mdash", "\xE2\x80\x94"},
      {"nbsp", "\xC2\xA0"},
      {"ndash", "\xE2\x80\x93"},
      {"ngE", "\xE2\x89\xA7\xCC\xB8"},
      {"ouml", "\xC3\xB6"},
      {"para", "\xC2\xB6"},
      {"plusmn", "\xC2\xB1"},
      {"pound", "\xC2\xA3"},
      {"quot", "\""},
      {"raquo", "\xC2\xBB"},
      {"rarr", "\xE2\x86\x92"},
      {"rdquo", "\xE2\x80\x9D"},
      {"reg", "\xC2\xAE"},
      {"rsquo", "\xE2\x80\x99"},
      {"sect", "\xC2\xA7"},
      {"spades", "\xE2\x99\xA0"},
      {"szlig", "\xC3\x9F"},
      {"times", "\xC3\x97"},
      {"trade", "\xE2\x84\xA2"},
      {"uarr", "\xE2\x86\x91"},
      {"uuml", "\xC3\xBC"},
      {"yen", "\xC2\xA5"},
  };

  // Binary search over sorted entity array (md4c-style lookup table)
  size_t lo = 0, hi = sizeof(kEntities) / sizeof(kEntities[0]);
  while (lo < hi) {
    size_t mid = lo + (hi - lo) / 2;
    int cmp_result = name.compare(kEntities[mid].name);
    if (cmp_result < 0) {
      hi = mid;
    } else if (cmp_result > 0) {
      lo = mid + 1;
    } else {
      return kEntities[mid].value;
    }
  }
  return "";
}

// Decode HTML entities (named, decimal, hex) and backslash escapes
// C++20: uses std::string_view::find_first_of for efficient character scanning
inline std::pmr::string DecodeEscapesAndEntities(std::string_view text) {
  // Fast path: check if any processing needed using find_first_of
  size_t first_trigger = text.find_first_of("\\&");
  if (first_trigger == std::string_view::npos) {
    return std::pmr::string(text);  // Zero-copy for common case
  }

  std::pmr::string result;
  result.reserve(text.size());

  size_t i = 0;
  while (i < text.size()) {
    // Find span of characters that don't need processing using find_first_of
    size_t span_start = i;
    size_t next_trigger = text.find_first_of("\\&", i);
    if (next_trigger != std::string_view::npos) {
      i = next_trigger;
    } else {
      // No more triggers - copy rest and done
      result.append(text.data() + span_start, text.size() - span_start);
      break;
    }

    // Batch copy the span
    if (i > span_start) {
      result.append(text.data() + span_start, i - span_start);
    }

    // Backslash escape
    if (text[i] == '\\' && i + 1 < text.size() &&
        IsAsciiPunctuation(text[i + 1])) {
      result += text[i + 1];
      i += 2;
      continue;
    }

    // HTML entity
    if (text[i] == '&' && i + 1 < text.size()) {
      size_t start = i + 1;

      // Numeric character reference
      if (text[start] == '#') {
        size_t num_start = start + 1;
        bool is_hex = false;

        if (num_start < text.size() &&
            (text[num_start] == 'x' || text[num_start] == 'X')) {
          is_hex = true;
          ++num_start;
        }

        size_t num_end = num_start;
        if (is_hex) {
          while (num_end < text.size() &&
                 detail::IsXDigit(static_cast<unsigned char>(text[num_end]))) {
            ++num_end;
          }
        } else {
          while (num_end < text.size() &&
                 detail::IsDigit(static_cast<unsigned char>(text[num_end]))) {
            ++num_end;
          }
        }

        // CommonMark allows 1-7 decimal or 1-6 hexadecimal digits.
        size_t digits = num_end - num_start;
        if (digits > 0 && digits <= (is_hex ? 6u : 7u) &&
            num_end < text.size() && text[num_end] == ';') {
          std::string_view num_sv(text.data() + num_start, digits);
          uint32_t code_point;
          if (ParseUint(num_sv, code_point, is_hex ? 16 : 10)) {
            result += CodePointToUtf8(code_point);
            i = num_end + 1;
            continue;
          }
        }
      } else {
        // Named entity
        size_t name_end = start;
        while (name_end < text.size() &&
               detail::IsAlnum(static_cast<unsigned char>(text[name_end]))) {
          ++name_end;
        }

        if (name_end > start && name_end < text.size() &&
            text[name_end] == ';') {
          std::string_view entity =
              LookupHtmlEntity(text.substr(start, name_end - start));
          if (!entity.empty()) {
            result += entity;
            i = name_end + 1;
            continue;
          }
        }
      }
    }

    // No escape/entity matched, add literal character
    result += text[i];
    ++i;
  }

  return result;
}

// Decode backslash escapes only (no entities)
// C++20: uses std::string_view::find for efficient scanning
inline std::pmr::string DecodeEscapes(std::string_view text) {
  // Fast path: check if any escapes present using find
  size_t first_backslash = text.find('\\');
  if (first_backslash == std::string_view::npos) {
    return std::pmr::string(text);  // No escapes, zero-copy
  }

  std::pmr::string result;
  result.reserve(text.size());

  size_t i = 0;
  while (i < text.size()) {
    // Find span without backslashes using find
    size_t next_backslash = text.find('\\', i);
    if (next_backslash != std::string_view::npos) {
      // Batch copy the span before the backslash
      result.append(text.data() + i, next_backslash - i);
      i = next_backslash;
    } else {
      // No more backslashes - copy rest and done
      result.append(text.data() + i, text.size() - i);
      break;
    }

    // Handle backslash
    if (i < text.size()) {
      if (i + 1 < text.size() && IsAsciiPunctuation(text[i + 1])) {
        result += text[i + 1];
        i += 2;
      } else {
        result += text[i];
        ++i;
      }
    }
  }

  return result;
}

// Classify a line as an HTML block start. Returns the block type (1-7) and,
// for types 1-5, the end condition (a line containing it terminates the block);
// for types 6-7 the end condition is empty (they end at a blank line). Returns
// type 0 if the line does not start an HTML block. `trimmed` is the line with
// leading whitespace removed.
inline std::pair<int, std::string_view> ClassifyHtmlBlock(
    std::string_view trimmed) {
  int block_type = 0;
  std::string_view end_condition;

  // Type 1: <script>, <pre>, <style>, <textarea>
  static constexpr std::array type1_tags = {
      std::pair(std::string_view("<script"), std::string_view("</script>")),
      std::pair(std::string_view("<pre"), std::string_view("</pre>")),
      std::pair(std::string_view("<style"), std::string_view("</style>")),
      std::pair(std::string_view("<textarea"), std::string_view("</textarea>"))};
  for (auto [open_prefix, close_cond] : type1_tags) {
    if (StartsWithInsensitive(trimmed, open_prefix)) {
      if (trimmed.size() == open_prefix.size()) {
        block_type = 1;
        end_condition = close_cond;
        break;
      }
      char next = trimmed[open_prefix.size()];
      if (next == ' ' || next == '>' || next == '\t' || next == '\n') {
        block_type = 1;
        end_condition = close_cond;
        break;
      }
    }
  }

  // Type 2: <!-- comment -->. CommonMark 0.31 starts the block on any line
  // beginning with `<!--` (including `<!-->` and `<!--->`). This must agree
  // with the paragraph-interruption checks, which also accept any `<!--`;
  // a mismatch leaves the block parser unable to consume the line.
  if (block_type == 0 && trimmed.starts_with("<!--")) {
    block_type = 2;
    end_condition = "-->";
  }

  // Type 3: <? processing instruction ?>
  if (block_type == 0 && trimmed.starts_with("<?")) {
    block_type = 3;
    end_condition = "?>";
  }

  // Type 4: <!DOCTYPE
  if (block_type == 0 && StartsWithInsensitive(trimmed, "<!doctype")) {
    block_type = 4;
    end_condition = ">";
  }

  // Type 5: <![CDATA[
  if (block_type == 0 && trimmed.starts_with("<![CDATA[")) {
    block_type = 5;
    end_condition = "]]>";
  }

  // Type 6: Block-level HTML tags
  static constexpr std::array type6_tags = {
      std::string_view("address"), std::string_view("article"),
      std::string_view("aside"), std::string_view("base"),
      std::string_view("basefont"), std::string_view("blockquote"),
      std::string_view("body"), std::string_view("caption"),
      std::string_view("center"), std::string_view("col"),
      std::string_view("colgroup"), std::string_view("dd"),
      std::string_view("details"), std::string_view("dialog"),
      std::string_view("dir"), std::string_view("div"),
      std::string_view("dl"), std::string_view("dt"),
      std::string_view("fieldset"), std::string_view("figcaption"),
      std::string_view("figure"), std::string_view("footer"),
      std::string_view("form"), std::string_view("frame"),
      std::string_view("frameset"), std::string_view("h1"),
      std::string_view("h2"), std::string_view("h3"), std::string_view("h4"),
      std::string_view("h5"), std::string_view("h6"), std::string_view("head"),
      std::string_view("header"), std::string_view("hr"),
      std::string_view("html"), std::string_view("iframe"),
      std::string_view("legend"), std::string_view("li"),
      std::string_view("link"), std::string_view("main"),
      std::string_view("menu"), std::string_view("menuitem"),
      std::string_view("nav"), std::string_view("noframes"),
      std::string_view("ol"), std::string_view("optgroup"),
      std::string_view("option"), std::string_view("p"),
      std::string_view("param"), std::string_view("search"),
      std::string_view("section"), std::string_view("summary"),
      std::string_view("table"), std::string_view("tbody"),
      std::string_view("td"), std::string_view("tfoot"),
      std::string_view("th"), std::string_view("thead"),
      std::string_view("title"), std::string_view("tr"),
      std::string_view("track"), std::string_view("ul")};

  if (block_type == 0) {
    bool is_closing = (trimmed.size() >= 2 && trimmed[1] == '/');
    size_t tag_start = is_closing ? 2 : 1;

    size_t tag_end = tag_start;
    while (tag_end < trimmed.size() &&
           (detail::IsAlnum(static_cast<unsigned char>(trimmed[tag_end])) ||
            trimmed[tag_end] == '-')) {
      ++tag_end;
    }

    if (tag_end > tag_start) {
      std::string_view tag_name_sv = trimmed.substr(tag_start, tag_end - tag_start);
      for (auto t : type6_tags) {
        if (StartsWithInsensitive(tag_name_sv, t) &&
            tag_name_sv.size() == t.size()) {
          if (tag_end < trimmed.size()) {
            char next = trimmed[tag_end];
            if (next == ' ' || next == '>' || next == '\t' || next == '/' ||
                next == '\n') {
              block_type = 6;
              break;
            }
          } else {
            block_type = 6;
            break;
          }
        }
      }
    }
  }

  // Type 7: Other HTML tags (complete tag followed only by whitespace)
  if (block_type == 0) {
    bool is_closing = (trimmed.size() >= 2 && trimmed[1] == '/');
    size_t tag_start = is_closing ? 2 : 1;

    if (tag_start < trimmed.size() &&
        detail::IsAlpha(static_cast<unsigned char>(trimmed[tag_start]))) {
      size_t tag_end = tag_start + 1;
      while (tag_end < trimmed.size() &&
             (detail::IsAlnum(static_cast<unsigned char>(trimmed[tag_end])) ||
              trimmed[tag_end] == '-')) {
        ++tag_end;
      }

      bool valid_tag = false;
      if (tag_end < trimmed.size()) {
        char next = trimmed[tag_end];
        valid_tag = (next == ' ' || next == '\t' || next == '/' || next == '>');
      }
      if (valid_tag) {
        if (is_closing) {
          size_t search_pos = tag_end;
          while (search_pos < trimmed.size() &&
                 (trimmed[search_pos] == ' ' || trimmed[search_pos] == '\t')) {
            ++search_pos;
          }
          if (search_pos < trimmed.size() && trimmed[search_pos] == '>') {
            bool only_whitespace = true;
            for (size_t j = search_pos + 1; j < trimmed.size(); ++j) {
              if (trimmed[j] != ' ' && trimmed[j] != '\t') {
                only_whitespace = false;
                break;
              }
            }
            if (only_whitespace) {
              block_type = 7;
            }
          }
        } else {
          size_t search_pos = tag_end;
          bool valid_attributes = true;
          bool need_whitespace = false;

          while (search_pos < trimmed.size() && valid_attributes) {
            char c = trimmed[search_pos];
            if (c == '>') {
              break;
            } else if (c == '/') {
              if (search_pos + 1 < trimmed.size() &&
                  trimmed[search_pos + 1] == '>') {
                search_pos++;
              } else {
                valid_attributes = false;
              }
            } else if (c == ' ' || c == '\t' || c == '\n') {
              ++search_pos;
              need_whitespace = false;
            } else if (!need_whitespace &&
                       (detail::IsAlpha(static_cast<unsigned char>(c)) ||
                        c == '_' || c == ':')) {
              ++search_pos;
              while (
                  search_pos < trimmed.size() &&
                  (detail::IsAlnum(
                       static_cast<unsigned char>(trimmed[search_pos])) ||
                   trimmed[search_pos] == '_' || trimmed[search_pos] == ':' ||
                   trimmed[search_pos] == '.' || trimmed[search_pos] == '-')) {
                ++search_pos;
              }
              while (search_pos < trimmed.size() &&
                     (trimmed[search_pos] == ' ' || trimmed[search_pos] == '\t' ||
                      trimmed[search_pos] == '\n')) {
                ++search_pos;
                need_whitespace = false;
              }
              if (search_pos < trimmed.size() && trimmed[search_pos] == '=') {
                ++search_pos;
                while (search_pos < trimmed.size() &&
                       (trimmed[search_pos] == ' ' || trimmed[search_pos] == '\t' ||
                        trimmed[search_pos] == '\n')) {
                  ++search_pos;
                }
                if (search_pos >= trimmed.size()) {
                  valid_attributes = false;
                } else if (trimmed[search_pos] == '"' ||
                           trimmed[search_pos] == '\'') {
                  char quote = trimmed[search_pos];
                  ++search_pos;
                  while (search_pos < trimmed.size() &&
                         trimmed[search_pos] != quote) {
                    ++search_pos;
                  }
                  if (search_pos >= trimmed.size()) {
                    valid_attributes = false;
                  } else {
                    ++search_pos;
                    need_whitespace = true;
                  }
                } else {
                  while (search_pos < trimmed.size()) {
                    char vc = trimmed[search_pos];
                    if (vc == ' ' || vc == '\t' || vc == '\n' || vc == '"' ||
                        vc == '\'' || vc == '=' || vc == '<' || vc == '>' ||
                        vc == '`') {
                      break;
                    }
                    ++search_pos;
                  }
                  need_whitespace = true;
                }
              }
            } else {
              valid_attributes = false;
            }
          }
          if (valid_attributes && search_pos < trimmed.size() &&
              trimmed[search_pos] == '>') {
            bool only_whitespace = true;
            for (size_t j = search_pos + 1; j < trimmed.size(); ++j) {
              if (trimmed[j] != ' ' && trimmed[j] != '\t') {
                only_whitespace = false;
                break;
              }
            }
            if (only_whitespace) {
              block_type = 7;
            }
          }
        }
      }
    }
  }

  return {block_type, end_condition};
}

}  // namespace detail

using detail::CaseFold;
using detail::IsAlnum;
using detail::IsAlpha;
using detail::IsDigit;
using detail::IsXDigit;
using detail::ParseUint;
using detail::StartsWithInsensitive;
using detail::StringContainsInsensitive;

// =============================================================================
// Inline Parser
// =============================================================================

class InlineParser {
 public:
  explicit InlineParser(const LinkRefMap* link_refs = nullptr,
                        std::deque<std::pmr::string>* string_storage = nullptr,
                        std::pmr::vector<InlineNode>* inline_pool = nullptr)
      : link_references_(link_refs),
        string_storage_(string_storage),
        inline_pool_(inline_pool) {}

  std::pmr::vector<InlineNodeId> Parse(std::string_view text) {
    text_ = text;
    pos_ = 0;
    // The scan caches below describe the previous text; start afresh.
    backtick_cache_.clear();
    comment_miss_ = cdata_miss_ = pi_miss_ = decl_miss_ =
        std::string_view::npos;
    dquote_miss_ = squote_miss_ = std::string_view::npos;
    return ParseInlines();
  }

  // Copy `s` into Document-owned storage and return a stable view of it.
  // Strings are packed into chunks (each one a string_storage entry that is
  // never appended past its reserved capacity, so it never reallocates and
  // earlier views stay valid) instead of one string_storage entry apiece.
  std::string_view StoreString(std::string_view s) {
    if (!chunk_ || chunk_->capacity() - chunk_->size() < s.size()) {
      ReserveStorage(s.size() > kMinStorageChunk ? s.size() : kMinStorageChunk);
    }
    size_t offset = chunk_->size();
    chunk_->append(s);
    return std::string_view(chunk_->data() + offset, s.size());
  }

  // Start a new storage chunk able to hold at least `n` bytes.
  void ReserveStorage(size_t n) {
    string_storage_->emplace_back().reserve(n);
    chunk_ = &string_storage_->back();
  }

  // GFM `autolink` extension. When true, www/url/email autolinks are recognised
  // in plain text (in addition to the CommonMark `<...>` autolinks, which are
  // always recognised). Off by default for plain CommonMark behaviour.
  bool enable_autolink = false;

  // GFM `strikethrough` extension. When true, `~`/`~~` delimiter runs are
  // recognised and wrapped in a <del> node. Off by default so `~` is treated as
  // plain text under plain CommonMark behaviour.
  bool enable_strikethrough = false;

  // `latexmath` extension (mirrors md4c's MD_FLAG_LATEXMATHSPANS). When true,
  // `$...$` and `$$...$$` runs are recognised as LaTeX math spans with
  // verbatim content. Off by default so `$` is plain text under plain
  // CommonMark behaviour.
  bool enable_latex_math = false;

 private:
  std::string_view text_;
  size_t pos_ = 0;
  const LinkRefMap* link_references_ = nullptr;
  std::deque<std::pmr::string>* string_storage_ = nullptr;
  std::pmr::string* chunk_ = nullptr;  // Current StoreString() chunk
  static constexpr size_t kMinStorageChunk = 1024;
  std::pmr::vector<InlineNode>* inline_pool_ = nullptr;

  // Backtick position cache (cmark optimization): remembers, per backtick run
  // length, the earliest position from which a scan found no closer. A later
  // opener of the same length cannot find one either, so it fails at once.
  // Reset for every Parse() call (positions refer to the current text).
  static constexpr size_t kMaxBacktickLength = 1000;
  struct BacktickCache {
    size_t position = 0;
    bool scanned = false;
  };
  std::pmr::vector<BacktickCache> backtick_cache_;

  // Raw-HTML scan caches: the earliest position from which a search for the
  // terminator of a comment (`-->`), CDATA section (`]]>`), processing
  // instruction (`?>`), declaration (`>`) or quoted attribute value failed.
  // A search from any later position fails too, so it is skipped; otherwise
  // every unclosed `<!--` would rescan to the end of the text (quadratic).
  size_t comment_miss_ = std::string_view::npos;
  size_t cdata_miss_ = std::string_view::npos;
  size_t pi_miss_ = std::string_view::npos;
  size_t decl_miss_ = std::string_view::npos;
  size_t dquote_miss_ = std::string_view::npos;
  size_t squote_miss_ = std::string_view::npos;

  // Find `needle` in the text at or after `from`, using and updating the
  // negative cache `miss` described above.
  size_t FindCached(std::string_view needle, size_t from, size_t& miss) const {
    if (from >= miss) return std::string_view::npos;
    size_t found = text_.find(needle, from);
    if (found == std::string_view::npos) miss = from;
    return found;
  }

  // Maximum number of characters inside the brackets of a link label.
  static constexpr size_t kMaxLinkLabelLength = 999;
  // Maximum nesting of unescaped parentheses in a link destination.
  static constexpr int kMaxLinkDestinationParens = 32;

  // Nesting depth of each pool node (0 for leaves, 1 + the deepest child for
  // containers), indexed by node ID; used to enforce kMaxInlineNesting. IDs
  // past the end have depth 0.
  std::pmr::vector<uint16_t> node_depth_;

  uint16_t Depth(InlineNodeId id) const {
    return id < node_depth_.size() ? node_depth_[id] : 0;
  }

  void SetDepth(InlineNodeId id, uint16_t depth) {
    if (id < node_depth_.size()) {
      node_depth_[id] = depth;
    } else if (depth != 0) {
      node_depth_.resize(static_cast<size_t>(id) + 1, 0);
      node_depth_[id] = depth;
    }
  }

  // Deepest node among ids[begin, end).
  uint16_t MaxDepth(const std::pmr::vector<InlineNodeId>& ids, size_t begin,
                    size_t end) const {
    uint16_t depth = 0;
    for (size_t i = begin; i < end; ++i) {
      uint16_t d = Depth(ids[i]);
      if (d > depth) depth = d;
    }
    return depth;
  }

  // Depth of a container node holding `children`.
  uint16_t ContainerDepth(
      const std::pmr::vector<InlineNodeId>& children) const {
    return static_cast<uint16_t>(MaxDepth(children, 0, children.size()) + 1);
  }

  uint16_t NodeDepth(const InlineNode& node) const {
    return std::visit(
        [this](const auto& n) -> uint16_t {
          if constexpr (requires { n.children; }) {
            return ContainerDepth(n.children);
          } else {
            return 0;
          }
        },
        node);
  }

  // Add a node to the pool and return its ID
  InlineNodeId AddToPool(InlineNode node) {
    uint16_t depth = NodeDepth(node);
    InlineNodeId id;
    if (!free_ids_.empty()) {
      id = free_ids_.back();
      free_ids_.pop_back();
      Node(id) = std::move(node);
    } else {
      id = static_cast<InlineNodeId>(inline_pool_->size());
      inline_pool_->push_back(std::move(node));
    }
    SetDepth(id, depth);
    return id;
  }

  // Access a pool node by ID. The reference is invalidated by the next pool
  // insertion.
  InlineNode& Node(InlineNodeId id) { return (*inline_pool_)[id]; }

  // Construct a node directly in the pool and append its ID to `ids`. Nodes
  // are created in the pool up front and the parser then works on 4-byte IDs,
  // so wrapping content in emphasis/links never moves the (large) nodes.
  template <typename T, typename... Args>
  void Emit(std::pmr::vector<InlineNodeId>& ids, Args&&... args) {
    InlineNodeId id;
    if (!free_ids_.empty()) {
      id = free_ids_.back();
      free_ids_.pop_back();
      Node(id).template emplace<T>(std::forward<Args>(args)...);
    } else {
      id = static_cast<InlineNodeId>(inline_pool_->size());
      inline_pool_->emplace_back(std::in_place_type<T>,
                                 std::forward<Args>(args)...);
    }
    ids.push_back(id);
    if constexpr (requires(T& t) { t.children; }) {
      SetDepth(id, ContainerDepth(std::get<T>(Node(id)).children));
    } else {
      SetDepth(id, 0);
    }
  }

  // Mark a node that is no longer referenced (e.g. the verbatim content of a
  // math span, a consumed delimiter) and, recursively, its children as free
  // so Emit()/AddToPool() reuse their slots instead of growing the pool.
  void Release(InlineNodeId id) {
    InlineNode& node = Node(id);
    std::visit(
        [this](auto& n) {
          if constexpr (requires { n.children; }) {
            for (InlineNodeId child : n.children) Release(child);
          }
        },
        node);
    node = Text();
    free_ids_.push_back(id);
  }

  // Copy ids[begin, end) into a new child list.
  static std::pmr::vector<InlineNodeId> Slice(
      const std::pmr::vector<InlineNodeId>& ids, size_t begin, size_t end) {
    return std::pmr::vector<InlineNodeId>(ids.begin() + begin,
                                          ids.begin() + end);
  }

  // Scratch buffers reused across Parse() calls so each inline container does
  // not pay for fresh allocations. ParseInlines() does not recurse, and link
  // content is fully consumed before the next link can form, so one buffer of
  // each kind suffices.
  std::pmr::vector<InlineNodeId> result_scratch_;
  std::pmr::vector<InlineNodeId> link_scratch_;
  std::pmr::vector<InlineNodeId> autolink_scratch_;
  std::pmr::vector<InlineNodeId> emphasis_scratch_;
  // Pending `[` / `![` openers: the index of the opener in the delimiter
  // stack and the depth of the deepest node emitted after it (maintained
  // incrementally, so the nesting check does not rescan the result).
  struct BracketEntry {
    size_t delimiter_index;
    uint16_t max_depth;
  };
  std::pmr::vector<BracketEntry> bracket_scratch_;
  // Pool slots of released (unreferenced) nodes, reused before the pool grows.
  std::pmr::vector<InlineNodeId> free_ids_;
  std::pmr::string label_scratch_;

  struct DelimiterNode {
    size_t pos;       // Index in result vector
    size_t text_pos;  // Position in original text (for extracting raw labels)
    size_t count;
    char delimiter;
    bool can_open;
    bool can_close;
    bool active;
  };
  std::pmr::vector<DelimiterNode> delimiter_scratch_;
  std::pmr::vector<DelimiterNode> link_delimiter_scratch_;

  // `latexmath` extension: a `$` run can be a math span opener only when it is
  // not preceded by an alphanumeric character (i.e. it is at the start of the
  // text or preceded by Unicode whitespace or punctuation), mirroring md4c.
  bool MathCanOpen(size_t run_start) const {
    if (run_start == 0) return true;
    size_t before_start = FindPrevCharStart(run_start);
    auto [before_cp, before_len] = detail::DecodeUtf8At(text_, before_start);
    (void)before_len;
    return detail::IsUnicodeWhitespaceCodepoint(before_cp) ||
           detail::IsUnicodePunctuation(before_cp);
  }

  // `latexmath` extension: a `$` run can be a math span closer only when it is
  // not followed by an alphanumeric character (i.e. it is at the end of the
  // text or followed by Unicode whitespace or punctuation), mirroring md4c.
  bool MathCanClose(size_t run_start, size_t run_length) const {
    size_t after_pos = run_start + run_length;
    if (after_pos >= text_.size()) return true;
    auto [after_cp, after_len] = detail::DecodeUtf8At(text_, after_pos);
    (void)after_len;
    return detail::IsUnicodeWhitespaceCodepoint(after_cp) ||
           detail::IsUnicodePunctuation(after_cp);
  }

  std::pmr::vector<InlineNodeId> ParseInlines() {
    // Use vector for cache locality - for typical document sizes the O(n)
    // shift cost is negligible compared to list's pointer-chasing overhead.
    std::pmr::vector<InlineNodeId>& result = result_scratch_;
    result.clear();

    std::pmr::vector<DelimiterNode>& delimiter_stack = delimiter_scratch_;
    delimiter_stack.clear();

    // Pending bracket openers (indices into delimiter_stack). The `[` openers
    // among the first `link_floor` of them were deactivated by a link formed
    // after them (links cannot contain links); `![` openers stay active.
    std::pmr::vector<BracketEntry>& brackets = bracket_scratch_;
    brackets.clear();
    size_t link_floor = 0;
    // Record a container node (link, image) just appended to `result` in the
    // innermost pending bracket's content depth.
    auto note_container = [&]() {
      if (brackets.empty()) return;
      uint16_t d = Depth(result.back());
      if (d > brackets.back().max_depth) brackets.back().max_depth = d;
    };

    // Track spans instead of accumulating text
    size_t text_start = 0;
    bool in_span = false;

    auto flush_text = [&]() {
      if (in_span && text_start < pos_) {
        Emit<Text>(result, text_.substr(text_start, pos_ - text_start));
        in_span = false;
      }
    };

    auto flush_text_trimmed = [&]() {
      // Flush text but trim trailing spaces (for soft breaks)
      if (in_span && text_start < pos_) {
        size_t end = pos_;
        while (end > text_start && text_[end - 1] == ' ') {
          --end;
        }
        if (end > text_start) {
          Emit<Text>(result, text_.substr(text_start, end - text_start));
        }
        in_span = false;
      }
    };

    auto start_span = [&]() {
      if (!in_span) {
        text_start = pos_;
        in_span = true;
      }
    };

    while (pos_ < text_.size()) {
      char c = text_[pos_];

      // Fast path: backslash escape (common in markdown)
      [[likely]] if (c == '\\' && pos_ + 1 < text_.size()) {
        char next = text_[pos_ + 1];
        if (detail::IsAsciiPunctuation(next)) {
          flush_text();
          Emit<Text>(result, text_.substr(pos_ + 1, 1));
          pos_ += 2;
          continue;
        } else if (next == '\n') {
          flush_text();
          Emit<HardBreak>(result);
          pos_ += 2;
          continue;
        }
      }

      // HTML entity or numeric character reference
      [[likely]] if (c == '&' && pos_ + 1 < text_.size()) {
        flush_text();
        auto entity_result = TryParseEntity();
        if (entity_result) {
          Emit<Text>(result, StoreString(*entity_result));
          continue;
        }
        start_span();
      }

      // Code span
      if (c == '`') {
        // Flush text BEFORE trying to parse, since TryParseCodeSpan advances
        // pos_
        flush_text();
        auto code_span = TryParseCodeSpan();
        if (code_span) {
          Emit<Code>(result, std::move(code_span->content));
          continue;
        }
        // Code span didn't match - include backticks in current text span
        size_t backtick_count = 0;
        while (pos_ + backtick_count < text_.size() &&
               text_[pos_ + backtick_count] == '`') {
          ++backtick_count;
        }
        // Start new span that includes the backticks
        start_span();
        pos_ += backtick_count;
        continue;
      }

      // Check for autolink
      if (c == '<') {
        // Flush text BEFORE trying to parse, since Try* functions advance pos_
        flush_text();
        auto autolink = TryParseAutolink();
        if (autolink) {
          Emit<Link>(result, std::move(*autolink));
          note_container();
          continue;
        }

        // Check for HTML tag
        auto html = TryParseHtmlInline();
        if (html) {
          Emit<HtmlInline>(result, html->content);
          continue;
        }
        // Neither autolink nor HTML - treat '<' as regular text
        start_span();
      }

      // Check for emphasis markers
      if (c == '*' || c == '_') {
        size_t run_start = pos_;
        size_t run_length = 0;
        while (pos_ + run_length < text_.size() &&
               text_[pos_ + run_length] == c) {
          ++run_length;
        }

        bool left_flanking = IsLeftFlanking(run_start, run_length);
        bool right_flanking = IsRightFlanking(run_start, run_length);

        bool can_open = left_flanking;
        bool can_close = right_flanking;

        if (c == '_') {
          can_open = left_flanking &&
                     (!right_flanking ||
                      (run_start > 0 &&
                       detail::IsAsciiPunctuation(text_[run_start - 1])));
          can_close =
              right_flanking &&
              (!left_flanking ||
               (run_start + run_length < text_.size() &&
                detail::IsAsciiPunctuation(text_[run_start + run_length])));
        }

        flush_text();

        // Add the delimiter characters as text - view into input
        Emit<Text>(result, text_.substr(pos_, run_length));

        // Add delimiter to stack
        delimiter_stack.emplace_back(result.size() - 1, pos_, run_length, c,
                                     can_open, can_close, true);

        pos_ += run_length;
        continue;
      }

      // Check for GFM `strikethrough` markers (only when the extension is on).
      // Mirrors cmark-gfm's strikethrough match(): a run of 1 or 2 tildes that
      // is left- or right-flanking becomes a delimiter; longer runs and
      // non-flanking runs are plain text. The whole maximal run is emitted as a
      // single text node (and a delimiter is pushed only when valid) so that a
      // sub-run of a longer run is never re-examined. can_open/can_close come
      // straight from the flanking flags (no underscore-style extra rules).
      if (c == '~' && enable_strikethrough) {
        size_t run_start = pos_;
        size_t run_length = 0;
        while (pos_ + run_length < text_.size() &&
               text_[pos_ + run_length] == '~') {
          ++run_length;
        }

        flush_text();

        // Add the tilde run as text - view into input
        Emit<Text>(result, text_.substr(pos_, run_length));

        if (run_length <= 2) {
          bool left_flanking = IsLeftFlanking(run_start, run_length);
          bool right_flanking = IsRightFlanking(run_start, run_length);
          if (left_flanking || right_flanking) {
            delimiter_stack.emplace_back(result.size() - 1, pos_, run_length,
                                         '~', left_flanking, right_flanking,
                                         true);
          }
        }

        pos_ += run_length;
        continue;
      }

      // Check for `latexmath` markers (only when the extension is on).
      // Mirrors md4c's MD_FLAG_LATEXMATHSPANS: a run of 1 or 2 dollar signs
      // that can open or close a math span becomes a delimiter; longer runs
      // and runs that can do neither are plain text. Matching (same run
      // length, no nesting, verbatim content) happens in ProcessEmphasis.
      if (c == '$' && enable_latex_math) {
        size_t run_start = pos_;
        size_t run_length = 0;
        while (pos_ + run_length < text_.size() &&
               text_[pos_ + run_length] == '$') {
          ++run_length;
        }

        flush_text();

        // Add the dollar run as text - view into input
        Emit<Text>(result, text_.substr(pos_, run_length));

        if (run_length <= 2) {
          bool can_open = MathCanOpen(run_start);
          bool can_close = MathCanClose(run_start, run_length);
          if (can_open || can_close) {
            delimiter_stack.emplace_back(result.size() - 1, pos_, run_length,
                                         '$', can_open, can_close, true);
          }
        }

        pos_ += run_length;
        continue;
      }

      // Check for link/image start
      if (c == '[' ||
          (c == '!' && pos_ + 1 < text_.size() && text_[pos_ + 1] == '[')) {
        bool is_image = (c == '!');
        flush_text();

        size_t bracket_text_pos = pos_;  // Position in original text
        size_t len = is_image ? 2 : 1;
        Emit<Text>(result, text_.substr(pos_, len));  // "![" or "["
        delimiter_stack.emplace_back(result.size() - 1, bracket_text_pos, 1,
                                     is_image ? '!' : '[', true, false, true);
        brackets.push_back({delimiter_stack.size() - 1, 0});
        pos_ += len;
        continue;
      }

      // Check for link close. As in cmark, only the most recent bracket
      // opener is considered: when it is inactive or no link can be formed,
      // it is dropped and the ']' is literal text.
      if (c == ']') {
        flush_text();

        auto literal_bracket = [&]() {
          Emit<Text>(result, text_.substr(pos_, 1));  // "]"
          ++pos_;
        };
        // Pop the innermost bracket; its content now belongs to the next one.
        auto pop_bracket = [&]() {
          uint16_t d = brackets.back().max_depth;
          brackets.pop_back();
          if (link_floor > brackets.size()) link_floor = brackets.size();
          if (!brackets.empty() && d > brackets.back().max_depth) {
            brackets.back().max_depth = d;
          }
        };

        if (brackets.empty()) {
          literal_bracket();
          continue;
        }
        auto opener =
            delimiter_stack.begin() +
            static_cast<std::ptrdiff_t>(brackets.back().delimiter_index);
        bool is_image = (opener->delimiter == '!');
        bool active =
            opener->active && (is_image || brackets.size() > link_floor);
        // Bound the nesting of links and images (an image's alt text also
        // embeds the alt text of the images nested in it).
        if (brackets.back().max_depth >= detail::kMaxInlineNesting) {
          active = false;
        }
        if (!active) {
          pop_bracket();
          literal_bracket();
          continue;
        }

        size_t saved_pos = pos_;
        ++pos_;  // Skip ']'

        // Inline link `(dest "title")` or full reference `[label]`.
        auto target = TryParseLinkTail();
        if (!target) {
          // Collapsed `[]` (consumed by TryParseLinkTail) or shortcut
          // reference: look up the bracket text itself. When a full reference
          // `[foo][bar]` failed because `bar` is undefined, there is no
          // fallback to the shortcut `[foo]` (per the CommonMark spec).
          bool is_collapsed_ref =
              (pos_ == saved_pos + 3 && saved_pos + 2 < text_.size() &&
               text_[saved_pos + 1] == '[' && text_[saved_pos + 2] == ']');
          bool had_full_ref_attempt =
              (saved_pos + 1 < text_.size() && text_[saved_pos + 1] == '[' &&
               !is_collapsed_ref);
          if (!had_full_ref_attempt) {
            if (!is_collapsed_ref) pos_ = saved_pos + 1;
            // Use the raw text (escapes preserved) as the label.
            size_t label_start = opener->text_pos + (is_image ? 2 : 1);
            size_t label_len = saved_pos - label_start;
            if (label_len <= kMaxLinkLabelLength) {
              target = LookupReference(text_.substr(label_start, label_len));
            }
          }
        }
        if (!target) {
          pos_ = saved_pos;
          pop_bracket();
          literal_bracket();
          continue;
        }

        // Build the link/image from the nodes after the opener.
        size_t opener_idx = opener->pos;
        size_t original_result_size = result.size();
        std::pmr::vector<InlineNodeId>& link_content = link_scratch_;
        link_content.assign(
            result.begin() + static_cast<std::ptrdiff_t>(opener_idx) + 1,
            result.end());
        result.resize(opener_idx + 1);

        // Resolve emphasis among the delimiters inside the link content.
        size_t content_offset = opener_idx + 1;
        std::pmr::vector<DelimiterNode>& link_delimiters =
            link_delimiter_scratch_;
        link_delimiters.clear();
        for (auto it = opener + 1; it != delimiter_stack.end(); ++it) {
          if (it->pos > opener_idx && it->pos < original_result_size) {
            DelimiterNode d = *it;
            d.pos -= content_offset;  // Adjust to link_content indices
            link_delimiters.emplace_back(d);
          }
        }
        ProcessEmphasis(link_content, link_delimiters);

        // Remove the opener text node.
        Release(result[opener_idx]);
        result.resize(opener_idx);

        if (is_image) {
          Image img;
          img.destination = std::move(target->first);
          img.title = std::move(target->second);
          AppendAltText(link_content, img.alt_text);
          uint16_t depth = ContainerDepth(link_content);
          for (InlineNodeId id : link_content) Release(id);
          Emit<Image>(result, std::move(img));
          SetDepth(result.back(), depth);
        } else {
          Link link;
          link.destination = std::move(target->first);
          link.title = std::move(target->second);
          link.children = Slice(link_content, 0, link_content.size());
          Emit<Link>(result, std::move(link));
        }

        // The delimiters inside the link were consumed with its content.
        delimiter_stack.erase(opener, delimiter_stack.end());
        brackets.pop_back();
        if (!is_image) {
          // No links inside links: deactivate every earlier `[` opener.
          link_floor = brackets.size();
        } else if (link_floor > brackets.size()) {
          link_floor = brackets.size();
        }
        note_container();
        continue;
      }

      // Line break: a hard break when the line ends in two or more spaces,
      // otherwise a soft break. Those trailing spaces are always the tail of
      // the current text span (every inline construct ends in a non-space
      // character), so they are dropped by the trimmed flush either way.
      if (c == '\n') {
        size_t space_count = 0;
        while (space_count < pos_ && text_[pos_ - 1 - space_count] == ' ') {
          ++space_count;
        }
        flush_text_trimmed();
        if (space_count >= 2) {
          Emit<HardBreak>(result);
        } else {
          Emit<SoftBreak>(result);
        }
        ++pos_;
        continue;
      }

      // Fast path: scan ahead past runs of regular characters (cmark-style bulk
      // scan) Handles >90% of characters in typical text without per-character
      // branching
      {
        size_t next_special = detail::FindNextSpecialChar(text_, pos_ + 1);
        start_span();
        pos_ = next_special;
      }
    }

    flush_text();

    // Process emphasis
    ProcessEmphasis(result, delimiter_stack);

    // GFM `autolink` extension: split text nodes that contain www/url/email
    // autolinks into Text/Link sequences.
    if (enable_autolink) {
      PostprocessAutolinks(result);
    }

    return Slice(result, 0, result.size());
  }

  // Find the start position of the UTF-8 character before the given position
  size_t FindPrevCharStart(size_t pos) const {
    if (pos == 0) return 0;
    size_t i = pos - 1;
    // Walk back through continuation bytes (10xxxxxx)
    while (i > 0 && (static_cast<unsigned char>(text_[i]) & 0xC0) == 0x80) {
      --i;
    }
    return i;
  }

  bool IsLeftFlanking(size_t pos, size_t length) {
    if (pos + length >= text_.size()) return false;

    // Get code point after the delimiter run
    auto [after_cp, after_len] = detail::DecodeUtf8At(text_, pos + length);

    // Not left-flanking if followed by Unicode whitespace
    if (detail::IsUnicodeWhitespaceCodepoint(after_cp)) return false;

    // Left-flanking if not followed by Unicode punctuation
    if (!detail::IsUnicodePunctuation(after_cp)) return true;

    // Followed by punctuation - check if preceded by whitespace or punctuation
    if (pos == 0) return true;  // Start of string is like whitespace

    // Get code point before the delimiter run
    size_t before_start = FindPrevCharStart(pos);
    auto [before_cp, before_len] = detail::DecodeUtf8At(text_, before_start);

    return detail::IsUnicodeWhitespaceCodepoint(before_cp) ||
           detail::IsUnicodePunctuation(before_cp);
  }

  bool IsRightFlanking(size_t pos, size_t length) {
    if (pos == 0) return false;

    // Get code point before the delimiter run
    size_t before_start = FindPrevCharStart(pos);
    auto [before_cp, before_len] = detail::DecodeUtf8At(text_, before_start);

    // Not right-flanking if preceded by Unicode whitespace
    if (detail::IsUnicodeWhitespaceCodepoint(before_cp)) return false;

    // Right-flanking if not preceded by Unicode punctuation
    if (!detail::IsUnicodePunctuation(before_cp)) return true;

    // Preceded by punctuation - check if followed by whitespace or punctuation
    if (pos + length >= text_.size())
      return true;  // End of string is like whitespace

    // Get code point after the delimiter run
    auto [after_cp, after_len] = detail::DecodeUtf8At(text_, pos + length);

    return detail::IsUnicodeWhitespaceCodepoint(after_cp) ||
           detail::IsUnicodePunctuation(after_cp);
  }

  Result<Code> TryParseCodeSpan() {
    size_t start = pos_;
    size_t backtick_count = 0;
    while (pos_ + backtick_count < text_.size() &&
           text_[pos_ + backtick_count] == '`') {
      ++backtick_count;
    }

    // Backtick position cache (cmark optimization): avoid rescanning for
    // closing backticks of a length we've already checked without success
    if (backtick_count <= kMaxBacktickLength &&
        backtick_cache_.size() > backtick_count &&
        backtick_cache_[backtick_count].scanned) {
      size_t cached_pos = backtick_cache_[backtick_count].position;
      if (cached_pos <= pos_) {
        // A scan from an earlier position found no closer of this length
        pos_ = start;
        return {};
      }
    }

    size_t content_start = pos_ + backtick_count;
    size_t search_pos = content_start;

    while (search_pos < text_.size()) {
      if (text_[search_pos] == '`') {
        size_t close_count = 0;
        while (search_pos + close_count < text_.size() &&
               text_[search_pos + close_count] == '`') {
          ++close_count;
        }
        if (close_count == backtick_count) {
          // Found matching closer
          std::pmr::string content(
              text_.substr(content_start, search_pos - content_start));

          // Normalize: replace newlines with spaces
          for (char& code_char : content) {
            if (code_char == '\n') code_char = ' ';
          }

          // Strip single leading and trailing space if both present
          if (content.size() >= 2 && content.front() == ' ' &&
              content.back() == ' ' &&
              content.find_first_not_of(' ') != std::string::npos) {
            content = content.substr(1, content.size() - 2);
          }

          pos_ = search_pos + close_count;
          return Code(std::move(content));
        }
        search_pos += close_count;
      } else {
        ++search_pos;
      }
    }

    // Cache the failed scan result for this backtick length
    if (backtick_count <= kMaxBacktickLength) {
      if (backtick_cache_.size() <= backtick_count) {
        backtick_cache_.resize(backtick_count + 1);
      }
      backtick_cache_[backtick_count].position = pos_;
      backtick_cache_[backtick_count].scanned = true;
    }

    pos_ = start;
    return {};
  }

  Result<Link> TryParseAutolink() {
    if (pos_ >= text_.size() || text_[pos_] != '<') [[unlikely]]
      return {};

    size_t start = pos_ + 1;
    size_t end = start;

    while (end < text_.size() && text_[end] != '>' && text_[end] != '<' &&
           text_[end] != '\n') {
      ++end;
    }

    if (end >= text_.size() || text_[end] != '>') [[unlikely]]
      return {};

    std::string_view content = text_.substr(start, end - start);
    if (content.empty()) [[unlikely]]
      return {};

    // Check for URI autolink: [a-zA-Z][a-zA-Z0-9+.-]{1,31}:
    // Manual parsing replaces std::regex for massive speedup
    if (detail::IsAlpha(static_cast<unsigned char>(content[0]))) {
      size_t i = 1;
      // Consume 1-31 chars of [a-zA-Z0-9+.-]
      while (i < content.size() && i - 1 < 32) {
        unsigned char c = static_cast<unsigned char>(content[i]);
        if (detail::IsAlnum(c) || c == '+' || c == '.' || c == '-') {
          ++i;
        } else {
          break;
        }
      }
      // Must have at least 1 scheme char and a colon
      if (i > 1 && i < content.size() && content[i] == ':') {
        // Rest must be non-whitespace, non-<>, non-null
        bool valid_uri = true;
        for (size_t j = i + 1; j < content.size(); ++j) {
          unsigned char c = static_cast<unsigned char>(content[j]);
          if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\0') {
            valid_uri = false;
            break;
          }
        }
        if (valid_uri) {
          pos_ = end + 1;
          Link link;
          link.destination = detail::EncodeUrl(content);
          link.children.push_back(AddToPool(Text(content)));
          return link;
        }
      }
    }

    // Check for email autolink: localpart@domain
    // Manual parsing replaces std::regex
    size_t at_pos = content.find('@');
    if (at_pos != std::string_view::npos && at_pos > 0) {
      // Validate local part: [a-zA-Z0-9.!#$%&'*+/=?^_`{|}~-]+
      static constexpr uint8_t kEmailLocalTable[256] = {
          0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
          0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
          0, 1, 0, 1, 1, 1, 1, 1, 0, 1, 1, 1, 1, 1, 1, 1,  // !"#$%&'*+/-
          1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 1, 0, 1, 0, 1,  // 0-9;=?~
          1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,  // A-O
          1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 1, 1,  // P-^
          0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,  // a-o
          1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 1, 1, 1, 0,  // p-z|}~
      };
      bool valid_local = true;
      for (size_t j = 0; j < at_pos; ++j) {
        if (!kEmailLocalTable[static_cast<unsigned char>(content[j])]) {
          valid_local = false;
          break;
        }
      }

      if (valid_local) {
        // Validate domain:
        // [a-zA-Z0-9](?:[a-zA-Z0-9-]{0,61}[a-zA-Z0-9])?(?:\.[...])*
        size_t d = at_pos + 1;
        bool valid_domain = true;
        if (d < content.size() &&
            detail::IsAlnum(static_cast<unsigned char>(content[d]))) {
          while (d < content.size()) {
            // Label start
            size_t label_start = d;
            if (!detail::IsAlnum(static_cast<unsigned char>(content[d]))) {
              valid_domain = false;
              break;
            }
            ++d;
            while (d < content.size() &&
                   (detail::IsAlnum(static_cast<unsigned char>(content[d])) ||
                    content[d] == '-')) {
              ++d;
            }
            // Label end must be alphanumeric
            if (d > label_start + 1 &&
                !detail::IsAlnum(static_cast<unsigned char>(content[d - 1]))) {
              valid_domain = false;
              break;
            }
            // Label length check (max 63)
            if (d - label_start > 63) {
              valid_domain = false;
              break;
            }
            if (d < content.size() && content[d] == '.') {
              ++d;
              // Next label must start with alnum
              if (d >= content.size() ||
                  !detail::IsAlnum(static_cast<unsigned char>(content[d]))) {
                valid_domain = false;
                break;
              }
            }
          }
        } else {
          valid_domain = false;
        }

        if (valid_domain && d == content.size()) {
          pos_ = end + 1;
          Link link;
          link.destination = "mailto:" + std::pmr::string(content);
          link.children.push_back(AddToPool(Text(content)));
          return link;
        }
      }
    }

    return {};
  }

  Result<std::pmr::string> TryParseEntity() {
    if (pos_ >= text_.size() || text_[pos_] != '&') [[unlikely]]
      return {};

    size_t start = pos_ + 1;

    // Numeric character reference
    if (start < text_.size() && text_[start] == '#') {
      size_t num_start = start + 1;
      bool is_hex = false;

      if (num_start < text_.size() &&
          (text_[num_start] == 'x' || text_[num_start] == 'X')) {
        is_hex = true;
        ++num_start;
      }

      size_t num_end = num_start;
      if (is_hex) {
        while (num_end < text_.size() &&
               detail::IsXDigit(static_cast<unsigned char>(text_[num_end]))) {
          ++num_end;
        }
      } else {
        while (num_end < text_.size() &&
               detail::IsDigit(static_cast<unsigned char>(text_[num_end]))) {
          ++num_end;
        }
      }

      // CommonMark allows 1-7 decimal or 1-6 hexadecimal digits; invalid
      // code points (out of range, surrogates, U+0000) become U+FFFD.
      size_t digits = num_end - num_start;
      if (digits > 0 && digits <= (is_hex ? 6u : 7u) &&
          num_end < text_.size() && text_[num_end] == ';') {
        std::string_view num_sv(text_.data() + num_start, digits);
        uint32_t code_point;
        if (ParseUint(num_sv, code_point, is_hex ? 16 : 10)) {
          pos_ = num_end + 1;
          return detail::CodePointToUtf8(code_point);
        }
      }
    } else if (start < text_.size()) {
      // Named entity
      size_t name_end = start;
      while (name_end < text_.size() &&
             detail::IsAlnum(static_cast<unsigned char>(text_[name_end]))) {
        ++name_end;
      }

      if (name_end > start && name_end < text_.size() &&
          text_[name_end] == ';') {
        std::string_view entity =
            detail::LookupHtmlEntity(text_.substr(start, name_end - start));
        if (!entity.empty()) {
          pos_ = name_end + 1;
          return std::pmr::string(entity);
        }
      }
    }

    return {};
  }

  Result<HtmlInline> TryParseHtmlInline() {
    if (pos_ >= text_.size() || text_[pos_] != '<') [[unlikely]]
      return {};

    size_t start = pos_;
    size_t end = pos_ + 1;

    // Simple HTML tag detection
    if (end >= text_.size()) [[unlikely]]
      return {};

    bool is_closing = (text_[end] == '/');
    if (is_closing) ++end;

    // Check for valid tag name start
    if (end >= text_.size() ||
        !detail::IsAlpha(static_cast<unsigned char>(text_[end]))) {
      // Check for comment, CDATA, processing instruction, or declaration
      if (text_.substr(pos_).starts_with("<!--")) {
        // Per CommonMark spec: <!--> and <!---> are valid (immediately closed)
        // Otherwise, find --> to close, but text must not start with > or ->
        if (pos_ + 4 < text_.size()) {
          char next = text_[pos_ + 4];
          if (next == '>') {
            // <!--> is a valid immediately-closed comment
            pos_ = pos_ + 5;
            return HtmlInline(text_.substr(start, pos_ - start));
          }
          if (next == '-' && pos_ + 5 < text_.size() &&
              text_[pos_ + 5] == '>') {
            // <!---> is a valid immediately-closed comment
            pos_ = pos_ + 6;
            return HtmlInline(text_.substr(start, pos_ - start));
          }
          // Normal comment: find -->
          end = FindCached("-->", pos_ + 4, comment_miss_);
          if (end != std::string_view::npos) {
            pos_ = end + 3;
            return HtmlInline(text_.substr(start, pos_ - start));
          }
        } else if (pos_ + 4 == text_.size()) {
          // Just "<!--" at end - not a valid comment, leave as text
        }
      }
      if (text_.substr(pos_).starts_with("<![CDATA[")) {
        end = FindCached("]]>", pos_ + 9, cdata_miss_);
        if (end != std::string_view::npos) {
          pos_ = end + 3;
          return HtmlInline(text_.substr(start, pos_ - start));
        }
      }
      if (text_.substr(pos_).starts_with("<?")) {
        end = FindCached("?>", pos_ + 2, pi_miss_);
        if (end != std::string_view::npos) {
          pos_ = end + 2;
          return HtmlInline(text_.substr(start, pos_ - start));
        }
      }
      if (text_.substr(pos_).starts_with("<!") && end + 1 < text_.size() &&
          std::isupper(static_cast<unsigned char>(text_[end + 1]))) {
        end = FindCached(">", pos_ + 2, decl_miss_);
        if (end != std::string_view::npos) {
          pos_ = end + 1;
          return HtmlInline(text_.substr(start, pos_ - start));
        }
      }
      return {};
    }

    // Parse tag name
    while (end < text_.size() &&
           (detail::IsAlnum(static_cast<unsigned char>(text_[end])) ||
            text_[end] == '-')) {
      ++end;
    }

    // Skip attributes and find closing - must follow HTML tag syntax
    // After tag name, expect whitespace, /, or > - anything else is invalid
    // For closing tags, only whitespace and > are allowed (no attributes)
    bool seen_whitespace = false;
    while (end < text_.size()) {
      char c = text_[end];
      if (c == '>') {
        pos_ = end + 1;
        return HtmlInline(text_.substr(start, pos_ - start));
      } else if (is_closing) {
        // Closing tags: only whitespace allowed before >
        if (c == ' ' || c == '\t' || c == '\n') {
          ++end;
        } else {
          return {};  // Invalid char in closing tag
        }
      } else if (c == '/') {
        // Self-closing: must be followed by >
        if (end + 1 < text_.size() && text_[end + 1] == '>') {
          pos_ = end + 2;
          return HtmlInline(text_.substr(start, pos_ - start));
        }
        return {};  // / not followed by >
      } else if (c == ' ' || c == '\t' || c == '\n') {
        seen_whitespace = true;
        ++end;
      } else if (seen_whitespace &&
                 (detail::IsAlpha(static_cast<unsigned char>(c)) || c == '_' ||
                  c == ':')) {
        // Start of attribute name (must be preceded by whitespace)
        seen_whitespace = false;  // Reset - need whitespace before next attr
        ++end;
        while (end < text_.size()) {
          char ac = text_[end];
          if (detail::IsAlnum(static_cast<unsigned char>(ac)) || ac == '_' ||
              ac == ':' || ac == '.' || ac == '-') {
            ++end;
          } else {
            break;
          }
        }
        // Optional attribute value
        // Skip whitespace
        while (end < text_.size() && (text_[end] == ' ' || text_[end] == '\t' ||
                                      text_[end] == '\n')) {
          seen_whitespace = true;
          ++end;
        }
        if (end < text_.size() && text_[end] == '=') {
          ++end;
          // Skip whitespace
          while (
              end < text_.size() &&
              (text_[end] == ' ' || text_[end] == '\t' || text_[end] == '\n')) {
            ++end;
          }
          if (end >= text_.size()) [[unlikely]]
            return {};
          if (text_[end] == '"' || text_[end] == '\'') {
            bool double_quoted = text_[end] == '"';
            end = FindCached(double_quoted ? "\"" : "'", end + 1,
                             double_quoted ? dquote_miss_ : squote_miss_);
            if (end == std::string_view::npos) [[unlikely]]
              return {};
            ++end;
          } else {
            // Unquoted value - no spaces, quotes, =, <, >, or `
            while (end < text_.size()) {
              char vc = text_[end];
              if (vc == ' ' || vc == '\t' || vc == '\n' || vc == '"' ||
                  vc == '\'' || vc == '=' || vc == '<' || vc == '>' ||
                  vc == '`') {
                break;
              }
              ++end;
            }
          }
        }
      } else {
        // Invalid character in tag
        return {};
      }
    }

    return {};
  }

  Result<std::pair<std::pmr::string, std::pmr::string>> TryParseLinkTail() {
    if (pos_ >= text_.size()) [[unlikely]]
      return {};

    // Inline link: (destination "title")
    if (text_[pos_] == '(') {
      ++pos_;
      SkipWhitespace();

      std::pmr::string destination;
      std::pmr::string title;

      // Parse destination
      if (pos_ < text_.size() && text_[pos_] == '<') {
        // Angle-bracketed destination (no line endings or unescaped '<')
        ++pos_;
        size_t dest_start = pos_;
        while (pos_ < text_.size() && text_[pos_] != '>' &&
               text_[pos_] != '\n' && text_[pos_] != '<') {
          if (text_[pos_] == '\\' && pos_ + 1 < text_.size()) {
            ++pos_;
          }
          ++pos_;
        }
        if (pos_ >= text_.size() || text_[pos_] != '>') [[unlikely]]
          return {};
        destination = detail::DecodeEscapesAndEntities(
            text_.substr(dest_start, pos_ - dest_start));
        ++pos_;
      } else if (pos_ < text_.size() && text_[pos_] != ')') {
        // Regular destination - only ASCII space/tab/newline are separators.
        // Parenthesis nesting is limited (as the spec allows, and cmark does)
        // so a run of unbalanced '(' cannot make every ']' rescan the text.
        size_t dest_start = pos_;
        int paren_depth = 0;
        while (pos_ < text_.size() && text_[pos_] != ' ' &&
               text_[pos_] != '\t' && text_[pos_] != '\n' &&
               text_[pos_] != '\r') {
          if (text_[pos_] == '(') {
            if (++paren_depth > kMaxLinkDestinationParens) return {};
          } else if (text_[pos_] == ')') {
            if (paren_depth == 0) break;
            --paren_depth;
          } else if (text_[pos_] == '\\' && pos_ + 1 < text_.size()) {
            ++pos_;
          }
          ++pos_;
        }
        destination = detail::DecodeEscapesAndEntities(
            text_.substr(dest_start, pos_ - dest_start));
      }

      SkipWhitespace();

      // Parse optional title
      if (pos_ < text_.size() &&
          (text_[pos_] == '"' || text_[pos_] == '\'' || text_[pos_] == '(')) {
        char open_char = text_[pos_];
        char close_char = open_char == '(' ? ')' : open_char;
        ++pos_;
        size_t title_start = pos_;
        while (pos_ < text_.size() && text_[pos_] != close_char) {
          // A parenthesized title cannot contain an unescaped '('.
          if (open_char == '(' && text_[pos_] == '(') return {};
          if (text_[pos_] == '\\' && pos_ + 1 < text_.size()) {
            ++pos_;
          }
          ++pos_;
        }
        if (pos_ >= text_.size()) [[unlikely]]
          return {};
        title = detail::DecodeEscapesAndEntities(
            text_.substr(title_start, pos_ - title_start));
        ++pos_;
        SkipWhitespace();
      }

      if (pos_ >= text_.size() || text_[pos_] != ')') [[unlikely]]
        return {};
      ++pos_;

      return std::make_pair(detail::EncodeUrl(destination), title);
    }

    // Reference link: [label] or []
    if (text_[pos_] == '[') {
      // A label ends at the first unescaped ']', cannot contain an unescaped
      // '[' and holds at most kMaxLinkLabelLength characters.
      ++pos_;
      size_t label_start = pos_;
      while (pos_ < text_.size() && text_[pos_] != ']') {
        if (text_[pos_] == '[') return {};
        if (text_[pos_] == '\\' && pos_ + 1 < text_.size()) {
          ++pos_;
        }
        ++pos_;
        if (pos_ - label_start > kMaxLinkLabelLength) return {};
      }

      if (pos_ >= text_.size()) [[unlikely]]
        return {};

      std::string_view label(text_.data() + label_start, pos_ - label_start);
      ++pos_;

      if (label.empty()) [[unlikely]] {
        // Collapsed reference - label comes from link text
        return {};  // Need to look up later
      }

      return LookupReference(label);
    }

    return {};
  }

  Result<std::pair<std::pmr::string, std::pmr::string>> LookupReference(
      std::string_view label) {
    if (!link_references_ || link_references_->empty()) return {};

    std::string_view normalized = label;
    if (!detail::IsNormalizedLinkLabel(label)) {
      detail::NormalizeLinkLabelInto(label, label_scratch_);
      normalized = label_scratch_;
    }
    auto it = link_references_->find(normalized);
    if (it != link_references_->end()) {
      return Result<std::pair<std::pmr::string, std::pmr::string>>::emplace(
          it->second);
    }
    return {};
  }

  void SkipWhitespace() {
    while (pos_ < text_.size() &&
           (text_[pos_] == ' ' || text_[pos_] == '\t' || text_[pos_] == '\n' ||
            text_[pos_] == '\r')) {
      ++pos_;
    }
  }

  // Append the plain-text rendering of `node_ids` (an image's alt text) to
  // `out`. Recursion depth is bounded by kMaxInlineNesting.
  void AppendAltText(const std::pmr::vector<InlineNodeId>& node_ids,
                     std::pmr::string& out) {
    for (InlineNodeId id : node_ids) {
      std::visit(
          [&out, this](const auto& arg) {
            using T = std::decay_t<decltype(arg)>;
            if constexpr (std::is_same_v<T, Text> || std::is_same_v<T, Code> ||
                          std::is_same_v<T, Math>) {
              out += arg.content;
            } else if constexpr (std::is_same_v<T, SoftBreak> ||
                                 std::is_same_v<T, HardBreak>) {
              out += ' ';
            } else if constexpr (std::is_same_v<T, Emphasis> ||
                                 std::is_same_v<T, Strong> ||
                                 std::is_same_v<T, Strikethrough> ||
                                 std::is_same_v<T, Link>) {
              AppendAltText(arg.children, out);
            } else if constexpr (std::is_same_v<T, Image>) {
              out += arg.alt_text;
            }
          },
          (*inline_pool_)[id]);
    }
  }

  // Resolve emphasis, strong emphasis, strikethrough and math spans among
  // `delimiters` (in text order; `pos` indexes `nodes`), replacing `nodes`
  // with the resulting node list.
  //
  // Mirrors cmark's process_emphasis in a single left-to-right pass: nodes
  // are copied to an output list as the delimiters are visited, and a match
  // collapses the output tail (the content after its opener) into a new
  // container node. Potential openers wait on a stack; a match removes the
  // openers between the opener and the closer (they can no longer match),
  // and `openers_bottom` remembers, per closer kind, below which point a
  // search already failed. Together these keep the pass linear in the
  // number of nodes and delimiters.
  void ProcessEmphasis(std::pmr::vector<InlineNodeId>& nodes,
                       std::pmr::vector<DelimiterNode>& delimiters) {
    if (delimiters.empty()) [[unlikely]]
      return;

    std::pmr::vector<InlineNodeId>& out = emphasis_scratch_;
    out.clear();
    out.reserve(nodes.size());

    // Pending openers: a delimiter index and the out position of its text.
    struct Opener {
      size_t index;
      size_t out_pos;
    };
    std::pmr::vector<Opener> openers;
    // `latexmath` extension: indices into `openers` of the pending `$`
    // openers (mirrors md4c's DOLLAR_OPENERS). A closer matches only the TOP
    // one, with an equal run length; math spans do not nest.
    std::pmr::vector<size_t> math_openers;
    auto truncate_openers = [&](size_t n) {
      openers.resize(n);
      while (!math_openers.empty() && math_openers.back() >= n) {
        math_openers.pop_back();
      }
    };

    // openers_bottom: per closer kind (delimiter character, whether the
    // closer can open, run length mod 3), the delimiter index below which no
    // opener was found.
    size_t openers_bottom[3][2][3] = {};
    auto kind_of = [](char c) { return c == '*' ? 0 : (c == '_' ? 1 : 2); };

    // Collapse out[opener_pos + 1, closer_pos) into `node`, keeping the
    // opener and closer text nodes; returns the closer's new out position.
    auto collapse = [&](size_t opener_pos, size_t closer_pos,
                        InlineNode&& node) {
      InlineNodeId closer_id = out[closer_pos];
      InlineNodeId id = AddToPool(std::move(node));
      out.resize(opener_pos + 1);
      out.push_back(id);
      out.push_back(closer_id);
      return out.size() - 1;
    };

    size_t in = 0;
    for (size_t di = 0; di < delimiters.size(); ++di) {
      DelimiterNode& closer = delimiters[di];
      if (closer.pos < in || closer.pos >= nodes.size()) [[unlikely]]
        continue;
      while (in <= closer.pos) out.push_back(nodes[in++]);
      size_t closer_pos = out.size() - 1;
      if (!closer.active) continue;

      if (closer.delimiter == '$') {
        if (closer.can_close && !math_openers.empty() &&
            delimiters[openers[math_openers.back()].index].count ==
                closer.count) {
          size_t stack_idx = math_openers.back();
          Opener o = openers[stack_idx];
          DelimiterNode& opener = delimiters[o.index];

          // The math content is verbatim: take the raw text between the two
          // runs (whatever inlines were parsed in it are discarded) and turn
          // it into text the same way md4c does - line breaks become a
          // single space, everything else (including multiple spaces) is
          // kept as-is.
          std::pmr::string content;
          size_t text_begin = opener.text_pos + opener.count;
          size_t text_end = closer.text_pos;
          for (size_t i = text_begin; i < text_end; ++i) {
            content.push_back(text_[i] == '\n' ? ' ' : text_[i]);
          }

          Node(out[o.out_pos]) = Text("");
          opener.active = false;
          Node(out[closer_pos]) = Text("");
          closer.active = false;
          for (size_t i = o.out_pos + 1; i < closer_pos; ++i) Release(out[i]);
          collapse(o.out_pos, closer_pos,
                   Math(std::move(content), closer.count == 2));

          // The opener and everything above it are consumed; discard all
          // other pending math openers too, as math spans do not nest.
          truncate_openers(stack_idx);
          for (size_t mi : math_openers)
            delimiters[openers[mi].index].active = false;
          math_openers.clear();
        } else if (closer.can_open) {
          openers.push_back({di, closer_pos});
          math_openers.push_back(openers.size() - 1);
        }
        continue;
      }

      if (closer.delimiter != '*' && closer.delimiter != '_' &&
          closer.delimiter != '~') {
        continue;
      }

      while (closer.can_close && closer.active) {
        size_t& bottom =
            openers_bottom[kind_of(closer.delimiter)][closer.can_open ? 1 : 0]
                          [closer.count % 3];
        // Find the nearest eligible opener.
        size_t k = openers.size();
        bool found = false;
        while (k > 0) {
          const Opener& o = openers[k - 1];
          if (o.index < bottom) break;
          const DelimiterNode& opener = delimiters[o.index];
          if (opener.active && opener.delimiter == closer.delimiter) {
            // Rule of 3: when either run can both open and close, the sum
            // of the run lengths must not be a multiple of 3 unless both
            // lengths are.
            bool rule_of_3 = ((opener.can_open && opener.can_close) ||
                              (closer.can_open && closer.can_close)) &&
                             (opener.count + closer.count) % 3 == 0 &&
                             opener.count % 3 != 0 && closer.count % 3 != 0;
            if (!rule_of_3) {
              found = true;
              break;
            }
          }
          --k;
        }
        // Past kMaxInlineNesting the delimiters stay literal text.
        if (found && MaxDepth(out, openers[k - 1].out_pos + 1, closer_pos) >=
                         detail::kMaxInlineNesting) {
          found = false;
        }
        if (!found) {
          bottom = di;
          break;
        }

        size_t stack_idx = k - 1;
        Opener o = openers[stack_idx];
        DelimiterNode& opener = delimiters[o.index];
        std::pmr::vector<InlineNodeId> children(
            out.begin() + static_cast<std::ptrdiff_t>(o.out_pos) + 1,
            out.begin() + static_cast<std::ptrdiff_t>(closer_pos));

        // GFM `strikethrough`: the opener and closer must have the SAME
        // number of tildes, and both whole runs are consumed. When the
        // counts differ, cmark-gfm still drops the two delimiters (and any
        // between them) without producing a node; the tildes remain in the
        // text and later closers can no longer match against them.
        if (closer.delimiter == '~') {
          if (opener.count == closer.count) {
            Node(out[o.out_pos]) = Text("");
            Node(out[closer_pos]) = Text("");
            Strikethrough strike;
            strike.children = std::move(children);
            closer_pos = collapse(o.out_pos, closer_pos, std::move(strike));
          }
          opener.active = false;
          closer.active = false;
          truncate_openers(stack_idx);
          break;
        }

        bool is_strong = opener.count >= 2 && closer.count >= 2;
        size_t delim_count = is_strong ? 2 : 1;
        InlineNode emph_node;
        if (is_strong) {
          Strong strong;
          strong.children = std::move(children);
          emph_node = std::move(strong);
        } else {
          Emphasis em;
          em.children = std::move(children);
          emph_node = std::move(em);
        }

        // Delimiters are consumed from the END of the opener run and the
        // BEGINNING of the closer run.
        if (opener.count > delim_count) {
          auto& opener_content = std::get<Text>(Node(out[o.out_pos])).content;
          opener_content = opener_content.substr(0, opener.count - delim_count);
          opener.count -= delim_count;
        } else {
          Node(out[o.out_pos]) = Text("");
          opener.active = false;
        }
        if (closer.count > delim_count) {
          auto& closer_content = std::get<Text>(Node(out[closer_pos])).content;
          closer_content = closer_content.substr(delim_count);
          closer.count -= delim_count;
        } else {
          Node(out[closer_pos]) = Text("");
          closer.active = false;
        }

        closer_pos = collapse(o.out_pos, closer_pos, std::move(emph_node));
        // Openers between the two runs can no longer match; neither can an
        // exhausted opener.
        truncate_openers(opener.active ? stack_idx + 1 : stack_idx);
      }

      if (closer.active && closer.can_open) {
        openers.push_back({di, closer_pos});
      }
    }
    while (in < nodes.size()) out.push_back(nodes[in++]);

    // Drop the text nodes of fully consumed delimiter runs.
    nodes.clear();
    for (InlineNodeId id : out) {
      const InlineNode& node = Node(id);
      if (std::holds_alternative<Text>(node) &&
          std::get<Text>(node).content.empty()) {
        Release(id);
        continue;
      }
      nodes.push_back(id);
    }
  }

  // ===========================================================================
  // GFM `autolink` extension
  //
  // Mirrors cmark-gfm's autolink.c. Recognises three kinds of autolink in plain
  // text (no angle brackets):
  //   * extended www autolink:  `www.` + valid domain (+ optional path)
  //   * extended url autolink:  `http://` / `https://` / `ftp://` + domain
  //   * extended email autolink: local-part@domain
  // This runs as a postprocess over the top-level inline nodes after emphasis,
  // splitting Text nodes into Text/Link sequences. It is intentionally limited
  // to top-level text (text inside links/images/emphasis is already in the pool
  // and, per the GFM spec tests, autolinks do not nest inside other inlines).
  // ===========================================================================

  enum class AutolinkKind { None, Www, Url, Email };

  // A character that terminates an autolink path (whitespace or '<').
  static bool IsAutolinkStopChar(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' ||
           c == '\v' || c == '<';
  }

  // A character valid in a domain/host segment: not whitespace and not
  // (ASCII) punctuation. Punctuation such as '/', '?', '=' ends the domain
  // portion and is picked up by the path scan instead.
  static bool IsValidHostChar(char c) {
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' ||
        c == '\v') {
      return false;
    }
    return !detail::IsAsciiPunctuation(c);
  }

  // A character allowed in the local part of an email address.
  static bool IsEmailLocalPartChar(char c) {
    return detail::IsAlnum(static_cast<unsigned char>(c)) || c == '.' ||
           c == '-' || c == '_' || c == '+';
  }

  // Case-insensitive match of one of the safe URL schemes at `pos`; returns the
  // number of scheme characters (including `://`) or 0 if no scheme matches.
  static size_t MatchScheme(std::string_view text, size_t pos) {
    auto matches = [&](const char* scheme) {
      size_t n = std::char_traits<char>::length(scheme);
      if (pos + n > text.size()) return false;
      for (size_t i = 0; i < n; ++i) {
        if (detail::CaseFold(static_cast<unsigned char>(text[pos + i])) !=
            scheme[i]) {
          return false;
        }
      }
      return true;
    };
    if (matches("https://")) return 8;
    if (matches("http://")) return 7;
    if (matches("ftp://")) return 6;
    return 0;
  }

  // Validates the domain starting at `start`; returns its length (0 if none).
  // Port of cmark's check_domain: rejects domains with an underscore in either
  // of the last two segments (host names, unlike domain names, disallow them),
  // and (unless allow_short) requires at least one period.
  static size_t CheckDomain(std::string_view data, size_t start,
                            bool allow_short) {
    size_t size = data.size() - start;
    if (size < 1) return 0;
    // The first character must be a valid host character too (e.g. not the
    // quote in `http://"x` or the space in `http:// x`).
    if (!IsValidHostChar(data[start])) return 0;
    int np = 0;
    int uscore1 = 0;
    int uscore2 = 0;
    size_t i = 1;  // data[start] is assumed valid (www.'s first char / host)
    for (; i < size - 1; ++i) {
      char c = data[start + i];
      if (c == '\\' && i + 1 < size - 1) {
        ++i;  // skip escaped character
        continue;
      }
      if (c == '_') {
        ++uscore2;
      } else if (c == '.') {
        uscore1 = uscore2;
        uscore2 = 0;
        ++np;
      } else if (!IsValidHostChar(c) && c != '-') {
        break;
      }
    }
    if ((uscore1 > 0 || uscore2 > 0) && np <= 10) {
      return 0;
    }
    if (allow_short) {
      return i;
    }
    return np ? i : 0;
  }

  // Trims trailing punctuation from an autolink that currently spans
  // text[base, link_end). Port of cmark's autolink_delim: handles trailing
  // punctuation, unbalanced closing parens, and entity-like `;` sequences.
  static size_t AutolinkDelim(std::string_view text, size_t base,
                              size_t link_end) {
    size_t closing = 0;
    size_t opening = 0;
    for (size_t i = base; i < link_end; ++i) {
      char c = text[i];
      if (c == '<') {
        link_end = i;
        break;
      } else if (c == '(') {
        ++opening;
      } else if (c == ')') {
        ++closing;
      }
    }
    while (link_end > base) {
      char c = text[link_end - 1];
      switch (c) {
        case ')':
          if (closing <= opening) {
            return link_end;
          }
          --closing;
          --link_end;
          break;
        case '?':
        case '!':
        case '.':
        case ',':
        case ':':
        case '*':
        case '_':
        case '~':
        case '\'':
        case '"':
          --link_end;
          break;
        case ';': {
          size_t rel_end = link_end - base;
          size_t new_end = (rel_end >= 2) ? rel_end - 2 : 0;
          while (new_end > 0 && detail::IsAlpha(static_cast<unsigned char>(
                                    text[base + new_end]))) {
            --new_end;
          }
          if (new_end < rel_end - 2 && new_end < rel_end &&
              text[base + new_end] == '&') {
            link_end = base + new_end;
          } else {
            --link_end;
          }
          break;
        }
        default:
          return link_end;
      }
    }
    return link_end;
  }

  // Tries to match an extended www autolink starting at `pos`.
  size_t TryMatchWww(std::string_view text, size_t pos) const {
    // An extended autolink can only come at the beginning of a line, after
    // whitespace, or after one of the delimiting characters * _ ~ ( .
    if (pos > 0) {
      char prev = text[pos - 1];
      if (!(prev == ' ' || prev == '\t' || prev == '\n' || prev == '\r' ||
            prev == '*' || prev == '_' || prev == '~' || prev == '(')) {
        return 0;
      }
    }
    if (pos + 4 > text.size() || text.compare(pos, 4, "www.") != 0) {
      return 0;
    }
    size_t domain_len = CheckDomain(text, pos, /*allow_short=*/false);
    if (domain_len == 0) return 0;
    size_t link_end = pos + domain_len;
    while (link_end < text.size() && !IsAutolinkStopChar(text[link_end])) {
      ++link_end;
    }
    link_end = AutolinkDelim(text, pos, link_end);
    if (link_end <= pos) return 0;
    return link_end - pos;
  }

  // Tries to match an extended url autolink starting at `pos`.
  size_t TryMatchUrl(std::string_view text, size_t pos) const {
    // The scheme must not be preceded by an alphanumeric, otherwise it is part
    // of a longer word (e.g. `xhttp://`).
    if (pos > 0 && detail::IsAlnum(static_cast<unsigned char>(text[pos - 1]))) {
      return 0;
    }
    size_t scheme_len = MatchScheme(text, pos);
    if (scheme_len == 0) return 0;
    size_t after_scheme = pos + scheme_len;
    size_t domain_len = CheckDomain(text, after_scheme, /*allow_short=*/true);
    if (domain_len == 0) return 0;
    size_t link_end = after_scheme + domain_len;
    while (link_end < text.size() && !IsAutolinkStopChar(text[link_end])) {
      ++link_end;
    }
    link_end = AutolinkDelim(text, pos, link_end);
    if (link_end <= pos) return 0;
    return link_end - pos;
  }

  // Tries to match an extended email autolink starting at `pos`.
  size_t TryMatchEmail(std::string_view text, size_t pos) const {
    // Local part: a run of local-part characters that must be followed by '@'.
    size_t i = pos;
    while (i < text.size() && IsEmailLocalPartChar(text[i])) {
      ++i;
    }
    if (i >= text.size() || text[i] != '@') return 0;
    size_t at = i;

    // Domain: alnum segments separated by '.', at least one period, and the
    // final character must be alphanumeric or '.' (a trailing '.' is dropped by
    // the delimiter trim below; a trailing '-' or '_' is invalid).
    size_t d = at + 1;
    int np = 0;
    while (d < text.size()) {
      unsigned char c = static_cast<unsigned char>(text[d]);
      if (detail::IsAlnum(c)) {
        ++d;
      } else if (c == '@') {
        return 0;  // a second '@' cannot appear in an email address
      } else if (c == '.' && d + 1 < text.size() &&
                 detail::IsAlnum(static_cast<unsigned char>(text[d + 1]))) {
        ++np;
        ++d;
      } else if (c == '-' || c == '_') {
        ++d;
      } else {
        break;
      }
    }
    size_t domain_len = d - (at + 1);
    if (domain_len < 2 || np == 0) return 0;
    char last = text[d - 1];
    if (!(detail::IsAlpha(static_cast<unsigned char>(last)) || last == '.')) {
      return 0;
    }
    size_t link_end = AutolinkDelim(text, at, d);
    if (link_end - at < 2) return 0;
    return link_end - pos;
  }

  // Attempts to match any extended autolink starting at `pos`; returns its
  // length and sets `kind`, or returns 0 with `kind == AutolinkKind::None`.
  size_t TryMatchAutolink(std::string_view text, size_t pos,
                          AutolinkKind& kind) const {
    unsigned char c = static_cast<unsigned char>(text[pos]);
    if (c == 'w') {
      size_t len = TryMatchWww(text, pos);
      if (len) {
        kind = AutolinkKind::Www;
        return len;
      }
    } else if (c == 'h' || c == 'f') {
      size_t len = TryMatchUrl(text, pos);
      if (len) {
        kind = AutolinkKind::Url;
        return len;
      }
    }
    if (detail::IsAlnum(c)) {
      // An email autolink can only begin at the start of a local-part run.
      bool run_start = (pos == 0) || !IsEmailLocalPartChar(text[pos - 1]);
      if (run_start) {
        size_t len = TryMatchEmail(text, pos);
        if (len) {
          kind = AutolinkKind::Email;
          return len;
        }
      }
    }
    kind = AutolinkKind::None;
    return 0;
  }

  std::pmr::string MakeAutolinkDestination(std::string_view matched,
                                           AutolinkKind kind) const {
    // Percent-encode like every other link destination.
    if (kind == AutolinkKind::Www) {
      return detail::EncodeUrl(std::pmr::string("http://") +
                               std::pmr::string(matched));
    }
    if (kind == AutolinkKind::Email) {
      return detail::EncodeUrl(std::pmr::string("mailto:") +
                               std::pmr::string(matched));
    }
    return detail::EncodeUrl(matched);
  }

  // Splits a single text span into Text/Link nodes, replacing any extended
  // autolinks with Link nodes (the matched text becomes the link label and the
  // scheme-appropriate URI becomes the destination).
  // `id` is a Text node; when it contains no autolink it is kept as is.
  void SplitAutolinks(InlineNodeId id, std::pmr::vector<InlineNodeId>& out) {
    std::string_view text = std::get<Text>(Node(id)).content;
    if (text.empty()) return;
    size_t text_start = 0;
    size_t pos = 0;
    bool matched_any = false;
    auto flush = [&](size_t upto) {
      if (text_start < upto) {
        Emit<Text>(out, text.substr(text_start, upto - text_start));
      }
      text_start = upto;
    };
    while (pos < text.size()) {
      AutolinkKind kind;
      size_t match = TryMatchAutolink(text, pos, kind);
      if (match > 0) {
        flush(pos);
        std::string_view matched = text.substr(pos, match);
        Link link;
        link.destination = MakeAutolinkDestination(matched, kind);
        link.children.push_back(AddToPool(Text(matched)));
        Emit<Link>(out, std::move(link));
        pos += match;
        text_start = pos;
        matched_any = true;
      } else {
        ++pos;
      }
    }
    if (!matched_any) {
      out.push_back(id);
      return;
    }
    flush(text.size());
    Release(id);
  }

  void PostprocessAutolinks(std::pmr::vector<InlineNodeId>& nodes) {
    // First consolidate adjacent text nodes. The inline parser splits text at
    // special characters (e.g. `_` emphasis markers that do not form emphasis),
    // which can fragment an autolink such as `a.b-c_d@a.b`. Merging the runs
    // restores the full span so detection sees it whole. Adjacent text nodes
    // are normally contiguous slices of the input, so the common case is a
    // cheap view extension; a persistent copy is only needed for the rare
    // non-contiguous run.
    // Each run of adjacent text nodes is merged at once (a single copy for a
    // non-contiguous run), keeping this linear in the text size.
    std::pmr::vector<InlineNodeId>& consolidated = autolink_scratch_;
    consolidated.clear();
    auto is_text = [this](InlineNodeId id) {
      return std::holds_alternative<Text>(Node(id));
    };
    for (size_t i = 0; i < nodes.size();) {
      InlineNodeId first = nodes[i];
      consolidated.push_back(first);
      size_t run_end = i + 1;
      if (!is_text(first)) {
        i = run_end;
        continue;
      }
      bool contiguous = true;
      size_t run_size = std::get<Text>(Node(first)).content.size();
      while (run_end < nodes.size() && is_text(nodes[run_end])) {
        std::string_view prev =
            std::get<Text>(Node(nodes[run_end - 1])).content;
        std::string_view cur = std::get<Text>(Node(nodes[run_end])).content;
        if (cur.data() != prev.data() + prev.size()) contiguous = false;
        run_size += cur.size();
        ++run_end;
      }
      if (run_end - i > 1) {
        std::string_view head = std::get<Text>(Node(first)).content;
        std::string_view merged_view;
        if (contiguous) {
          merged_view = std::string_view(head.data(), run_size);
        } else {
          std::pmr::string merged;
          merged.reserve(run_size);
          for (size_t j = i; j < run_end; ++j) {
            merged.append(std::get<Text>(Node(nodes[j])).content);
          }
          merged_view = StoreString(merged);
        }
        std::get<Text>(Node(first)).content = merged_view;
        for (size_t j = i + 1; j < run_end; ++j) Release(nodes[j]);
      }
      i = run_end;
    }

    // Then split any text nodes that contain extended autolinks.
    nodes.clear();
    for (InlineNodeId id : consolidated) {
      if (std::holds_alternative<Text>(Node(id))) {
        SplitAutolinks(id, nodes);
      } else {
        nodes.push_back(id);
      }
    }
  }
};

// =============================================================================
// Block Parser
// =============================================================================

class BlockParser {
 public:
  // GFM `table` extension. When false (default, CommonMark mode) tables are
  // not recognized and the corresponding syntax is left as plain paragraphs.
  bool enable_tables = false;

  // GFM `autolink` extension. When true, www/url/email autolinks are
  // recognised in plain text. The CommonMark `<...>` autolinks are always
  // recognised regardless of this flag.
  bool enable_autolink = false;

  // GFM `strikethrough` extension. When true, `~`/`~~` delimiter runs are
  // wrapped in a <del> node. Off by default for plain CommonMark behaviour.
  bool enable_strikethrough = false;

  // `latexmath` extension (mirrors md4c's MD_FLAG_LATEXMATHSPANS). When true,
  // `$...$` and `$$...$$` runs are recognised as LaTeX math spans. Off by
  // default for plain CommonMark behaviour.
  bool enable_latex_math = false;

  // GFM `tasklist` extension. When true, a list item whose first line begins
  // with a `[ ]`, `[x]` or `[X]` marker (followed by a space) is treated as a
  // task list item and rendered with a checkbox. Off by default for plain
  // CommonMark behaviour.
  bool enable_tasklist = false;

  // Internal: describes the last top-level block produced by the most recent
  // Parse()/ParseBlocksInto() call. Used by StreamingMarkdownParser to decide
  // where (if anywhere) the settled buffer can be cut without re-rendering.
  // Line indices are 0-based into the input's lines (detail::LineBuffer).
  bool has_last_top_block = false;
  size_t last_top_block_start_line = 0;
  size_t last_top_block_end_line = 0;    // exclusive
  bool last_top_block_terminated = true;  // cannot absorb a further line

  // Bytes of container content re-parsed by nested parsers during the most
  // recent Parse()/ParseBlocksInto() (a measure of its cost beyond the input
  // size, used by the streaming parsers to pace re-parsing).
  size_t nested_content_bytes() const { return nested_bytes_owned_; }

  BlockParser() = default;

  // Parser whose transient buffers (line offsets, paragraph line lists) come
  // from `scratch`; used for the nested parsers of container blocks so they
  // share the enclosing Parse()'s scratch arena.
  explicit BlockParser(std::pmr::memory_resource* scratch)
      : lines_(OrDefault(scratch)),
        para_lines_(OrDefault(scratch)),
        scratch_(scratch) {}

  Document Parse(std::string_view input) { return Parse(input, LinkRefMap()); }

  // Parse input as a fresh document, but pre-seed its link reference map with
  // `initial_refs` (references defined earlier, e.g. in a previous streaming
  // chunk). Already-defined labels win over later duplicates (first definition
  // wins), so seeding preserves document order across chunks.
  Document Parse(std::string_view input, const LinkRefMap& initial_refs) {
    return Parse(input, LinkRefMap(initial_refs));
  }

  // As above, taking ownership of `initial_refs` (no copy). The returned
  // Document's link_references holds them plus the definitions in `input`.
  Document Parse(std::string_view input, LinkRefMap&& initial_refs) {
    // Transient parse buffers (container contents re-parsed by nested
    // parsers, their line tables, paragraph text awaiting inline parsing) are
    // bump-allocated from an arena released when the parse ends. Declared
    // before `doc` so it outlives the Document if an exception unwinds.
    std::pmr::monotonic_buffer_resource arena(input.size() / 2 + 1024);
    ScratchScope scratch_scope(this, &arena);
    Document doc;
    doc.link_references = std::move(initial_refs);
    std::pmr::vector<BlockNode> top_blocks;
    ParseBlocksInto(input, doc, top_blocks);
    doc.children = std::move(top_blocks);

    ParseAllInlines(doc, input.size());

    return doc;
  }

  // Third pass: parse the inline content of every paragraph, heading and
  // table cell.
  void ParseAllInlines(Document& doc, size_t input_size) {
    // Pre-size the node pool from the input length (inline-heavy text yields
    // roughly one node per 10 bytes) so it rarely has to grow, as every growth
    // step moves each node.
    doc.inline_nodes.reserve(input_size / 10 + 16);
    InlineParser inline_parser(&doc.link_references, &doc.string_storage,
                               &doc.inline_nodes);
    inline_parser.enable_autolink = enable_autolink;
    inline_parser.enable_strikethrough = enable_strikethrough;
    inline_parser.enable_latex_math = enable_latex_math;

    // Inline nodes view their text in Document-owned storage. Size the first
    // storage chunk to hold all block text at once (every block lives in
    // either doc.children or doc.block_nodes).
    size_t total = 0;
    auto count = [&total](const BlockNode& block) {
      if (auto* para = std::get_if<Paragraph>(&block)) {
        total += para->raw_content.size();
      } else if (auto* heading = std::get_if<Heading>(&block)) {
        total += heading->raw_content.size();
      } else if (auto* table = std::get_if<Table>(&block)) {
        for (const auto& row : table->rows) {
          for (const auto& cell : row.cells) total += cell.raw_content.size();
        }
      }
    };
    for (const auto& block : doc.children) count(block);
    for (const auto& block : doc.block_nodes) count(block);
    if (total > 0) inline_parser.ReserveStorage(total);

    ParseInlines(doc.children, inline_parser);

    // No string in the returned Document may refer to the scratch arena,
    // which dies with the Parse() call (every raw_content reached above has
    // already been reset; this catches any that were not).
    if (scratch_) {
      auto release = [this](BlockNode& block) {
        std::pmr::string* raw = nullptr;
        if (auto* para = std::get_if<Paragraph>(&block)) {
          raw = &para->raw_content;
        } else if (auto* heading = std::get_if<Heading>(&block)) {
          raw = &heading->raw_content;
        }
        if (raw && raw->get_allocator().resource() == scratch_) {
          ResetString(*raw);
        }
      };
      for (auto& block : doc.children) release(block);
      for (auto& block : doc.block_nodes) release(block);
    }
  }

  // Copy a block's raw text into Document storage and release the original
  // (raw_content is only needed until inlines are parsed).
  static std::string_view StableText(std::pmr::string& raw,
                                     InlineParser& parser) {
    std::string_view stable = parser.StoreString(raw);
    ResetString(raw);
    return stable;
  }

  // Extract only the link reference definitions from `input` into `out`,
  // without building blocks or inlines. First definition wins for duplicates.
  void ExtractLinkRefs(std::string_view input, LinkRefMap& out) {
    if (input.find('[') == std::string_view::npos) return;
    lines_.Reset(input);
    line_idx_ = 0;
    ExtractLinkReferences(out);
  }

  void ParseBlocksInto(std::string_view input, Document& doc,
                       std::pmr::vector<BlockNode>& blocks,
                       bool input_no_nulls = false) {
    if (depth_ == 0) {
      // A new document: reset the nested content budget.
      nested_bytes_owned_ = 0;
      nested_bytes_ = &nested_bytes_owned_;
      nested_budget_ = input.size() > (static_cast<size_t>(-1) -
                                       detail::kNestedContentSlack) /
                                          detail::kMaxNestedContentFactor
                           ? static_cast<size_t>(-1)
                           : input.size() * detail::kMaxNestedContentFactor +
                                 detail::kNestedContentSlack;
    }
    doc_ = &doc;
    lines_.Reset(input, input_no_nulls);
    line_idx_ = 0;
    parent_link_refs_ = &doc.link_references;

    // The top-level parse reaches this point before any nested parser, so
    // size the pools for nested blocks and top-level blocks from its line
    // count (every growth step moves each block).
    if (doc.block_nodes.capacity() == 0) {
      doc.block_nodes.reserve(lines_.size() / 4 + 8);
      blocks.reserve(blocks.size() + lines_.size() / 8 + 8);
    }

    // Number of real lines: LineBuffer appends one empty artifact line when
    // the input ends with a line terminator; that artifact is not content.
    real_line_count_ = lines_.size();
    if (!input.empty() && (input.back() == '\n' || input.back() == '\r')) {
      --real_line_count_;
    }

    has_last_top_block = false;
    last_top_block_terminated = true;

    // First pass: extract link reference definitions (skip if no '[' present)
    if (input.find('[') != std::string_view::npos) {
      ExtractLinkReferences(doc.link_references);
    }

    // Second pass: parse blocks
    line_idx_ = 0;
    ParseBlocks(blocks);
  }

 private:
  // LineBuffer provides cache-friendly storage: contiguous offset array
  // pointing into single buffer, vs vector<string> with many allocations
  detail::LineBuffer lines_;
  size_t line_idx_ = 0;
  LinkRefMap* parent_link_refs_ = nullptr;
  Document* doc_ = nullptr;

  // Real (non-artifact) line count of the current input, set by
  // ParseBlocksInto. A block that consumes lines up to (but not past) the
  // end of the real content may still grow and is reported as unterminated.
  size_t real_line_count_ = 0;
  bool last_fence_found_closing_ = true;
  bool last_html_end_condition_found_ = true;
  size_t last_table_start_line_ = 0;

  // Scratch line list reused by ParseParagraph.
  std::pmr::vector<std::string_view> para_lines_;

  // Arena for transient parse buffers (see Parse()); null when parsing
  // through ParseBlocksInto() directly, in which case the default resource
  // is used.
  std::pmr::memory_resource* scratch_ = nullptr;

  static std::pmr::memory_resource* OrDefault(
      std::pmr::memory_resource* resource) {
    return resource ? resource : std::pmr::get_default_resource();
  }

  std::pmr::memory_resource* Scratch() const { return OrDefault(scratch_); }

  // Container nesting depth of this parser (0 for the top-level document).
  int depth_ = 0;

  // Bytes of container content handed to nested parsers so far in this parse
  // (shared by all nested parsers through `nested_bytes_`) and the limit
  // (see kMaxNestedContentFactor).
  // (Set by ParseBlocksInto on the top-level parser before any use.)
  size_t nested_bytes_owned_ = 0;
  size_t* nested_bytes_ = nullptr;
  size_t nested_budget_ = 0;

  // Parse the content of a container block (block quote, list item, details
  // section) into `out` with a nested parser that inherits this parser's
  // settings. Past kMaxBlockNesting (each level recurses) or the nested
  // content budget, the content is not parsed as blocks but kept as one
  // paragraph of literal text.
  void ParseNestedInto(std::string_view content,
                       std::pmr::vector<BlockNode>& out) {
    bool flatten =
        depth_ + 1 >= detail::kMaxBlockNesting ||
        (nested_bytes_ && content.size() > nested_budget_ - *nested_bytes_);
    if (!flatten && nested_bytes_) *nested_bytes_ += content.size();
    if (flatten) [[unlikely]] {
      std::pmr::string raw(Scratch());
      raw.reserve(content.size());
      size_t pos = 0;
      while (pos < content.size()) {
        size_t end = content.find('\n', pos);
        if (end == std::string_view::npos) end = content.size();
        std::string_view line = detail::Trim(content.substr(pos, end - pos));
        if (!line.empty() && line[0] == '\x01') line.remove_prefix(1);
        if (!line.empty()) {
          if (!raw.empty()) raw += '\n';
          raw.append(line);
        }
        pos = end + 1;
      }
      if (raw.empty()) return;
      Paragraph para;
      para.raw_content = std::move(raw);
      out.emplace_back(std::in_place_type<Paragraph>, std::move(para));
      return;
    }
    BlockParser nested(scratch_);
    nested.enable_tables = enable_tables;
    nested.enable_autolink = enable_autolink;
    nested.enable_strikethrough = enable_strikethrough;
    nested.enable_latex_math = enable_latex_math;
    nested.enable_tasklist = enable_tasklist;
    nested.depth_ = depth_ + 1;
    nested.nested_bytes_ = nested_bytes_;
    nested.nested_budget_ = nested_budget_;
    nested.ParseBlocksInto(content, *doc_, out, /*input_no_nulls=*/true);
  }

  // Installs `arena` as the scratch resource for the duration of a Parse().
  struct ScratchScope {
    ScratchScope(BlockParser* parser, std::pmr::memory_resource* arena)
        : parser_(parser), saved_(parser->scratch_) {
      parser->scratch_ = arena;
    }
    ~ScratchScope() { parser_->scratch_ = saved_; }
    ScratchScope(const ScratchScope&) = delete;
    ScratchScope& operator=(const ScratchScope&) = delete;
    BlockParser* parser_;
    std::pmr::memory_resource* saved_;
  };

  // Replace `s` with an empty string on the default resource (plain
  // assignment would keep an arena allocator, as polymorphic_allocator does
  // not propagate on assignment).
  static void ResetString(std::pmr::string& s) {
    std::destroy_at(&s);
    std::construct_at(&s);
  }

  // Set a block's raw_content to `lines` joined, allocated from the scratch
  // arena (it is only needed until inlines are parsed).
  void SetRawContent(std::pmr::string& raw,
                     const std::pmr::vector<std::string_view>& lines) {
    std::destroy_at(&raw);
    std::construct_at(&raw, JoinLines(lines, Scratch()));
  }

  // Record the block just pushed onto `blocks` as the last top-level block.
  // `iter_start_line` is the line on which the producing loop iteration began;
  // for tables the block actually starts later (at the header row), so the
  // line remembered by TryBuildTable is used instead.
  void RecordLastTopBlock(const std::pmr::vector<BlockNode>& blocks,
                          size_t iter_start_line) {
    const BlockNode& last = blocks.back();
    last_top_block_start_line =
        std::holds_alternative<Table>(last) ? last_table_start_line_
                                            : iter_start_line;
    last_top_block_end_line = line_idx_;

    // A block that stopped before the end of the input was terminated by a
    // real line (blank line, interrupt, closing fence, end condition) and
    // cannot absorb further lines.
    bool terminated = last_top_block_end_line < real_line_count_;
    std::visit(
        [this, &terminated](const auto& n) {
          using T = std::decay_t<decltype(n)>;
          if constexpr (std::is_same_v<T, Heading> ||
                        std::is_same_v<T, ThematicBreak>) {
            // Atomic single-line blocks are final as soon as their line has
            // been seen.
            terminated = true;
          } else if constexpr (std::is_same_v<T, CodeBlock>) {
            if (n.is_fenced) {
              // A fence consumes to its closing fence or to end-of-input;
              // only the closing fence makes it final (a fence closed on the
              // final real line has end_line >= real_line_count_).
              terminated = last_fence_found_closing_;
            }
            // Indented code: general rule (blank line terminates, EOF does not).
          } else if constexpr (std::is_same_v<T, DetailsBlock>) {
            // A section closed by </details> is complete; one that reached
            // end-of-input without the closing tag may still grow.
            terminated = n.closed;
          } else if constexpr (std::is_same_v<T, HtmlBlock>) {
            if (n.block_type <= 5) {
              // Types 1-5 consume to the end-condition line or end-of-input.
              terminated = last_html_end_condition_found_;
            }
            // Types 6-7: general rule (a real blank line terminates; the
            // trailing-newline artifact line does not).
          }
          // Paragraph, List, BlockQuote, Table: general rule. Lists and
          // block quotes can be re-opened after a blank line by a further
          // item/quote line, so reaching end-of-input leaves them open.
        },
        last);
    last_top_block_terminated = terminated;
    has_last_top_block = true;
  }

  bool AtEnd() const { return line_idx_ >= lines_.size(); }

  std::string_view CurrentLine() const {
    if (AtEnd()) return "";
    return lines_[line_idx_];
  }

  void Advance() {
    if (!AtEnd()) ++line_idx_;
  }

  // Helper to find closing bracket accounting for escapes
  size_t FindClosingBracket(std::string_view s, size_t start) {
    for (size_t i = start; i < s.size(); ++i) {
      if (s[i] == '\\' && i + 1 < s.size()) {
        ++i;  // Skip escaped character
      } else if (s[i] == ']') {
        return i;
      }
    }
    return std::string_view::npos;
  }

  // Check if brackets are balanced in a label string
  bool IsLabelBracketsBalanced(std::string_view label) {
    int depth = 0;
    for (size_t i = 0; i < label.size(); ++i) {
      if (label[i] == '\\' && i + 1 < label.size()) {
        ++i;  // Skip escaped character
        continue;
      }
      if (label[i] == '[') ++depth;
      if (label[i] == ']') --depth;
      if (depth < 0) [[unlikely]]
        return false;
    }
    return depth == 0;
  }

  // Helper to parse destination with balanced parentheses
  std::pair<std::pmr::string, size_t> ParseLinkDestination(std::string_view s) {
    if (s.empty()) [[unlikely]]
      return {"", 0};

    if (s[0] == '<') {
      // Angle-bracket destination
      for (size_t i = 1; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 1 < s.size()) {
          ++i;
        } else if (s[i] == '>') {
          return {std::pmr::string(s.substr(1, i - 1)), i + 1};
        } else if (s[i] == '<' || s[i] == '\n') [[unlikely]] {
          return {"", 0};
        }
      }
      return {"", 0};  // Unclosed angle bracket - unlikely
    }

    // Regular destination with balanced parentheses
    int paren_depth = 0;
    size_t end = 0;
    while (end < s.size()) {
      char c = s[end];
      if (c == '\\' && end + 1 < s.size()) {
        end += 2;
      } else if (c == '(') {
        ++paren_depth;
        ++end;
      } else if (c == ')') {
        if (paren_depth == 0) break;
        --paren_depth;
        ++end;
      } else if (detail::IsUnicodeWhitespace(c) ||
                 static_cast<unsigned char>(c) < 0x20) {
        break;
      } else {
        ++end;
      }
    }
    if (paren_depth != 0) [[unlikely]]
      return {"", 0};
    return {std::pmr::string(s.substr(0, end)), end};
  }

  void ExtractLinkReferences(LinkRefMap& refs) {
    // Track fenced code block state
    bool in_fenced_code = false;
    char fence_char = 0;
    size_t fence_length = 0;

    // Track if previous line had content (to reject link refs after content)
    bool prev_line_had_content = false;

    while (!AtEnd()) {
      std::string_view line = CurrentLine();
      int indent = detail::CountIndent(line);
      auto trimmed = detail::TrimLeft(line);

      // Handle fenced code blocks - check for fence opening/closing
      if (!in_fenced_code && indent < 4 && !trimmed.empty() &&
          (trimmed[0] == '`' || trimmed[0] == '~')) {
        char potential_fence = trimmed[0];
        size_t potential_len = 0;
        while (potential_len < trimmed.size() &&
               trimmed[potential_len] == potential_fence) {
          ++potential_len;
        }
        if (potential_len >= 3) {
          // Check that backtick fence doesn't have backticks in info string
          if (potential_fence == '~' ||
              trimmed.substr(potential_len).find('`') ==
                  std::string_view::npos) {
            in_fenced_code = true;
            fence_char = potential_fence;
            fence_length = potential_len;
            Advance();
            continue;
          }
        }
      }

      if (in_fenced_code) {
        // Check for closing fence
        if (indent < 4 && !trimmed.empty() && trimmed[0] == fence_char) {
          size_t close_len = 0;
          while (close_len < trimmed.size() &&
                 trimmed[close_len] == fence_char) {
            ++close_len;
          }
          if (close_len >= fence_length) {
            // Check rest is whitespace only
            bool is_close = true;
            for (size_t j = close_len; j < trimmed.size(); ++j) {
              if (trimmed[j] != ' ' && trimmed[j] != '\t') {
                is_close = false;
                break;
              }
            }
            if (is_close) {
              in_fenced_code = false;
            }
          }
        }
        Advance();
        continue;
      }

      // Skip indented lines (4+ spaces)
      if (indent >= 4) {
        prev_line_had_content = !detail::IsBlankLine(line);
        Advance();
        continue;
      }

      // Check for blank line - resets prev_line_had_content
      if (detail::IsBlankLine(line)) {
        prev_line_had_content = false;
        Advance();
        continue;
      }

      // Check for block-level elements that end a block (not continuable to
      // paragraph) These reset prev_line_had_content to false
      bool is_block_element = false;
      if (trimmed.starts_with('#')) {
        // ATX heading
        size_t hash_count = 0;
        while (hash_count < trimmed.size() && trimmed[hash_count] == '#')
          ++hash_count;
        if (hash_count <= 6 &&
            (hash_count >= trimmed.size() || trimmed[hash_count] == ' ' ||
             trimmed[hash_count] == '\t')) {
          is_block_element = true;
        }
      }
      if (!is_block_element && trimmed.starts_with('>')) {
        is_block_element = true;  // Block quote
      }
      if (!is_block_element && trimmed.size() >= 3) {
        // Check for thematic break
        char first = trimmed[0];
        if (first == '*' || first == '-' || first == '_') {
          int count = 0;
          bool valid = true;
          for (char ch : trimmed) {
            if (ch == first)
              ++count;
            else if (ch != ' ' && ch != '\t') {
              valid = false;
              break;
            }
          }
          if (valid && count >= 3) is_block_element = true;
        }
      }

      if (is_block_element) {
        prev_line_had_content = false;
        Advance();
        continue;
      }

      // Link ref definitions cannot follow non-blank paragraph content
      if (prev_line_had_content) {
        Advance();
        continue;
      }

      if (!trimmed.starts_with('[')) {
        prev_line_had_content = true;
        Advance();
        continue;
      }

      // Find closing bracket (handling escapes, may span multiple lines)
      size_t start_line = line_idx_;
      std::pmr::string label_raw;
      std::string_view rest;
      bool found_close = false;

      // First check if closing bracket is on the same line
      size_t close_bracket = FindClosingBracket(trimmed, 1);
      if (close_bracket != std::string_view::npos &&
          close_bracket + 1 < trimmed.size() &&
          trimmed[close_bracket + 1] == ':') {
        label_raw = std::pmr::string(trimmed.substr(1, close_bracket - 1));
        rest = detail::TrimLeft(trimmed.substr(close_bracket + 2));
        found_close = true;
        Advance();
      } else {
        // Try multi-line label (label can span multiple lines)
        label_raw = std::pmr::string(trimmed.substr(1));  // Content after '['
        Advance();

        // Collect lines until we find ']:' (limit to reasonable number of
        // lines)
        int lines_collected = 0;
        while (!AtEnd() && lines_collected < 50) {
          std::string_view next_line = CurrentLine();
          auto next_trimmed = detail::TrimLeft(next_line);

          // Look for ]: at the start of a line (allowing leading whitespace)
          size_t colon_pos = next_trimmed.find("]:");
          if (colon_pos != std::string_view::npos) {
            // Found it - check if there's no unescaped ] before this position
            bool valid = true;
            for (size_t j = 0; j < colon_pos; ++j) {
              if (next_trimmed[j] == '\\' && j + 1 < colon_pos) {
                ++j;  // Skip escaped char
              } else if (next_trimmed[j] == ']') {
                valid = false;  // Unmatched ] before ]:
                break;
              }
            }
            if (valid) {
              label_raw += '\n';
              label_raw += std::pmr::string(next_trimmed.substr(0, colon_pos));
              rest = detail::TrimLeft(next_trimmed.substr(colon_pos + 2));
              found_close = true;
              Advance();
              break;
            }
          }

          // Check for blank line - that ends the attempt
          if (detail::IsBlankLine(next_line)) {
            break;
          }

          // Continue collecting label content
          label_raw += '\n';
          label_raw += std::pmr::string(next_trimmed);
          Advance();
          ++lines_collected;
        }
      }

      if (!found_close) {
        line_idx_ = start_line;
        prev_line_had_content = true;
        Advance();
        continue;
      }

      // Check that label has balanced brackets
      if (!IsLabelBracketsBalanced(label_raw)) {
        line_idx_ = start_line;
        prev_line_had_content = true;
        Advance();
        continue;
      }

      // Normalize label for matching (WITHOUT decoding escapes per CommonMark
      // spec)
      std::pmr::string normalized = detail::NormalizeLinkLabel(label_raw);
      if (normalized.empty()) [[unlikely]] {
        line_idx_ = start_line;
        prev_line_had_content = true;
        Advance();
        continue;
      }

      // Destination might be on next line (no indentation required)
      if (rest.empty() || detail::IsBlankLine(rest)) {
        if (AtEnd()) {
          line_idx_ = start_line;
          prev_line_had_content = true;
          Advance();
          continue;
        }
        std::string_view next_line = CurrentLine();
        if (detail::IsBlankLine(next_line)) {
          line_idx_ = start_line;
          prev_line_had_content = true;
          Advance();
          continue;
        }
        rest = detail::TrimLeft(next_line);
        Advance();
      }

      // Parse destination
      auto [dest_raw, dest_len] = ParseLinkDestination(rest);
      if (dest_raw.empty() && dest_len == 0 && !rest.empty() &&
          rest[0] != '<') {
        line_idx_ = start_line;
        prev_line_had_content = true;
        Advance();
        continue;
      }
      std::pmr::string destination = detail::DecodeEscapesAndEntities(dest_raw);
      auto rest_after_dest = rest.substr(dest_len);
      bool has_whitespace_before_title =
          rest_after_dest.size() != detail::TrimLeft(rest_after_dest).size();
      rest = detail::TrimLeft(rest_after_dest);

      // Parse optional title (may be on same line or next line)
      std::pmr::string title;
      bool title_valid = true;
      bool title_on_new_line = false;
      size_t pre_title_line = line_idx_;

      if (rest.empty() && !AtEnd()) {
        std::string_view next_line = CurrentLine();
        // Title can be on next line if next line starts with title char
        auto trimmed_next = detail::TrimLeft(next_line);
        if (!detail::IsBlankLine(next_line) &&
            (trimmed_next.size() > 0 &&
             (trimmed_next[0] == '"' || trimmed_next[0] == '\'' ||
              trimmed_next[0] == '('))) {
          title_on_new_line = true;
          rest = trimmed_next;
          Advance();
        }
      }

      if (!rest.empty() &&
          (rest[0] == '"' || rest[0] == '\'' || rest[0] == '(')) {
        // Title must be separated from destination by whitespace (or be on new
        // line)
        if (!title_on_new_line && !has_whitespace_before_title) {
          title_valid = false;
        } else {
          char open_char = rest[0];
          char close_char = (open_char == '(') ? ')' : open_char;
          std::pmr::string title_content;
          size_t i = 1;

          // Title can span multiple lines
          while (title_valid) {
            while (i < rest.size()) {
              if (rest[i] == '\\' && i + 1 < rest.size()) {
                title_content += rest[i];
                title_content += rest[i + 1];
                i += 2;
              } else if (rest[i] == close_char) {
                // Found closing - rest must be whitespace only
                auto after = detail::TrimLeft(rest.substr(i + 1));
                if (!after.empty()) {
                  title_valid = false;
                } else {
                  title = detail::DecodeEscapesAndEntities(title_content);
                }
                goto title_done;
              } else if (rest[i] == '\n') {
                title_content += rest[i];
                ++i;
              } else {
                title_content += rest[i];
                ++i;
              }
            }
            // Continue to next line
            if (AtEnd()) {
              title_valid = false;
              break;
            }
            std::string_view next_line = CurrentLine();
            if (detail::IsBlankLine(next_line)) {
              title_valid = false;
              break;
            }
            title_content += '\n';
            rest = next_line;
            i = 0;
            Advance();
          }
        title_done:;
        }  // close else block for whitespace check
      } else if (!rest.empty()) {
        // Non-whitespace after destination with no title
        title_valid = false;
      }

      // If destination parsing returned length 0 (not angle brackets) and no
      // destination, it's invalid But angle bracket destinations like <>
      // returning empty string with length > 0 are valid
      if (dest_raw.empty() && dest_len == 0) {
        line_idx_ = start_line;
        prev_line_had_content = true;
        Advance();
        continue;
      }

      // If title parsing failed but was on a new line, roll back just the title
      // line and accept the link ref without a title
      if (!title_valid && title_on_new_line) {
        line_idx_ = pre_title_line;
        title.clear();
      } else if (!title_valid) {
        // Title on same line failed - invalid link ref
        line_idx_ = start_line;
        prev_line_had_content = true;
        Advance();
        continue;
      }

      // Only add if not already defined (first definition wins)
      if (refs.find(normalized) == refs.end()) {
        refs.emplace(std::move(normalized),
                     std::pair{detail::EncodeUrl(destination), title});
      }
      // After successful link ref extraction, next line starts fresh
      prev_line_had_content = false;
    }
    // Reset for block parsing
    line_idx_ = 0;
  }

  void ParseBlocks(std::pmr::vector<BlockNode>& blocks) {
    while (!AtEnd()) {
      std::string_view line = CurrentLine();

      // Skip blank lines
      if (detail::IsBlankLine(line)) {
        Advance();
        continue;
      }

      const size_t iter_start_line = line_idx_;

      // Check for various block types
      if (TryParseThematicBreak(blocks)) {
        RecordLastTopBlock(blocks, iter_start_line);
        continue;
      }

      if (TryParseAtxHeading(blocks)) {
        RecordLastTopBlock(blocks, iter_start_line);
        continue;
      }

      if (TryParseFencedCodeBlock(blocks)) {
        RecordLastTopBlock(blocks, iter_start_line);
        continue;
      }

      if (TryParseHtmlBlock(blocks)) {
        RecordLastTopBlock(blocks, iter_start_line);
        continue;
      }

      if (TryParseBlockQuote(blocks)) {
        RecordLastTopBlock(blocks, iter_start_line);
        continue;
      }

      if (TryParseList(blocks)) {
        RecordLastTopBlock(blocks, iter_start_line);
        continue;
      }

      if (TryParseIndentedCodeBlock(blocks)) {
        RecordLastTopBlock(blocks, iter_start_line);
        continue;
      }

      // Check for link reference definition (skip it, already extracted)
      if (TrySkipLinkReferenceDefinition()) {
        continue;
      }

      // Default: paragraph (may include setext heading, or a GFM table)
      ParseParagraph(blocks);
      RecordLastTopBlock(blocks, iter_start_line);
    }
  }

  bool TrySkipLinkReferenceDefinition() {
    std::string_view line = CurrentLine();
    int indent = detail::CountIndent(line);
    if (indent >= 4) return false;

    auto trimmed = detail::TrimLeft(line);
    if (!trimmed.starts_with('[')) return false;

    // Find closing bracket (handling escapes, may span multiple lines)
    size_t start_line = line_idx_;
    std::pmr::string label_raw;
    std::string_view rest;
    bool found_close = false;

    // First check if closing bracket is on the same line
    size_t close_bracket = FindClosingBracket(trimmed, 1);
    if (close_bracket != std::string_view::npos &&
        close_bracket + 1 < trimmed.size() &&
        trimmed[close_bracket + 1] == ':') {
      label_raw = std::pmr::string(trimmed.substr(1, close_bracket - 1));
      rest = detail::TrimLeft(trimmed.substr(close_bracket + 2));
      found_close = true;
      Advance();
    } else {
      // Try multi-line label
      label_raw = std::pmr::string(trimmed.substr(1));
      Advance();

      int lines_collected = 0;
      while (!AtEnd() && lines_collected < 50) {
        std::string_view next_line = CurrentLine();
        auto next_trimmed = detail::TrimLeft(next_line);

        size_t colon_pos = next_trimmed.find("]:");
        if (colon_pos != std::string_view::npos) {
          bool valid = true;
          for (size_t j = 0; j < colon_pos; ++j) {
            if (next_trimmed[j] == '\\' && j + 1 < colon_pos) {
              ++j;
            } else if (next_trimmed[j] == ']') {
              valid = false;
              break;
            }
          }
          if (valid) {
            label_raw += '\n';
            label_raw += std::pmr::string(next_trimmed.substr(0, colon_pos));
            rest = detail::TrimLeft(next_trimmed.substr(colon_pos + 2));
            found_close = true;
            Advance();
            break;
          }
        }

        if (detail::IsBlankLine(next_line)) {
          break;
        }

        label_raw += '\n';
        label_raw += std::pmr::string(next_trimmed);
        Advance();
        ++lines_collected;
      }
    }

    if (!found_close) {
      line_idx_ = start_line;
      return false;
    }

    // Check that label has balanced brackets
    if (!IsLabelBracketsBalanced(label_raw)) {
      line_idx_ = start_line;
      return false;
    }

    std::pmr::string normalized = detail::NormalizeLinkLabel(label_raw);
    if (normalized.empty()) [[unlikely]] {
      line_idx_ = start_line;
      return false;
    }

    // Destination might be on next line (no indentation required)
    if (rest.empty() || detail::IsBlankLine(rest)) {
      if (AtEnd()) {
        line_idx_ = start_line;
        return false;
      }
      std::string_view next_line = CurrentLine();
      if (detail::IsBlankLine(next_line)) {
        line_idx_ = start_line;
        return false;
      }
      rest = detail::TrimLeft(next_line);
      Advance();
    }

    // Parse destination
    auto [dest_raw, dest_len] = ParseLinkDestination(rest);
    if (dest_raw.empty() && dest_len == 0 && !rest.empty() && rest[0] != '<') {
      line_idx_ = start_line;
      return false;
    }
    auto rest_after_dest = rest.substr(dest_len);
    bool has_whitespace_before_title =
        rest_after_dest.size() != detail::TrimLeft(rest_after_dest).size();
    rest = detail::TrimLeft(rest_after_dest);

    // Parse optional title (may be on next line)
    bool title_valid = true;
    bool title_on_new_line = false;
    size_t pre_title_line = line_idx_;

    if (rest.empty() && !AtEnd()) {
      std::string_view next_line = CurrentLine();
      auto trimmed_next = detail::TrimLeft(next_line);
      if (!detail::IsBlankLine(next_line) &&
          (trimmed_next.size() > 0 &&
           (trimmed_next[0] == '"' || trimmed_next[0] == '\'' ||
            trimmed_next[0] == '('))) {
        title_on_new_line = true;
        rest = trimmed_next;
        Advance();
      }
    }

    if (!rest.empty() &&
        (rest[0] == '"' || rest[0] == '\'' || rest[0] == '(')) {
      // Title must be separated from destination by whitespace (or be on new
      // line)
      if (!title_on_new_line && !has_whitespace_before_title) {
        title_valid = false;
      } else {
        char open_char = rest[0];
        char close_char = (open_char == '(') ? ')' : open_char;
        size_t i = 1;

        while (title_valid) {
          while (i < rest.size()) {
            if (rest[i] == '\\' && i + 1 < rest.size()) {
              i += 2;
            } else if (rest[i] == close_char) {
              auto after = detail::TrimLeft(rest.substr(i + 1));
              if (!after.empty()) {
                title_valid = false;
              }
              goto skip_title_done;
            } else {
              ++i;
            }
          }
          if (AtEnd()) {
            title_valid = false;
            break;
          }
          std::string_view next_line = CurrentLine();
          if (detail::IsBlankLine(next_line)) {
            title_valid = false;
            break;
          }
          rest = next_line;
          i = 0;
          Advance();
        }
      skip_title_done:;
      }  // close else block for whitespace check
    } else if (!rest.empty()) {
      title_valid = false;
    }

    // If title failed but was on a new line, roll back just title and accept
    // without title
    if (!title_valid && title_on_new_line) {
      line_idx_ = pre_title_line;
    } else if (!title_valid) {
      line_idx_ = start_line;
      return false;
    }

    return true;
  }

  bool TryParseThematicBreak(std::pmr::vector<BlockNode>& blocks) {
    std::string_view line = CurrentLine();
    int indent = detail::CountIndent(line);
    if (indent >= 4) [[unlikely]]
      return false;

    auto trimmed = detail::TrimLeft(line);
    if (trimmed.empty()) [[unlikely]]
      return false;

    char marker = trimmed[0];
    if (marker != '-' && marker != '*' && marker != '_') [[unlikely]]
      return false;

    int count = 0;
    for (char c : trimmed) {
      if (c == marker) {
        ++count;
      } else if (c != ' ' && c != '\t') {
        return false;
      }
    }

    if (count >= 3) {
      Advance();
      blocks.emplace_back(std::in_place_type<ThematicBreak>);
      return true;
    }

    // =============================================================================
    // Inline Parser Special Character Lookup Table (cmark-style optimization)
    // Marks characters that require special handling in inline parsing
    // This enables bulk scanning of "safe" text runs in the hot path
    // =============================================================================

    return false;
  }

  bool TryParseAtxHeading(std::pmr::vector<BlockNode>& blocks) {
    std::string_view line = CurrentLine();
    int indent = detail::CountIndent(line);
    if (indent >= 4) [[unlikely]]
      return false;

    auto trimmed = detail::TrimLeft(line);
    if (trimmed.empty() || trimmed[0] != '#') [[unlikely]]
      return false;

    int level = 0;
    size_t i = 0;
    while (i < trimmed.size() && trimmed[i] == '#') {
      ++level;
      ++i;
    }

    if (level > 6) [[unlikely]]
      return false;
    if (i < trimmed.size() && trimmed[i] != ' ' && trimmed[i] != '\t') {
      return false;
    }

    // Skip space after #'s
    while (i < trimmed.size() && (trimmed[i] == ' ' || trimmed[i] == '\t')) {
      ++i;
    }

    // Get content, stripping the optional closing sequence: one run of '#'s
    // that is preceded by a space or tab (or is the whole content) and
    // followed only by spaces or tabs. Only a single run is removed, so
    // `# foo # #` keeps `foo #`.
    std::string_view rest = trimmed.substr(i);
    size_t end = rest.find_last_not_of(" \t");
    rest = (end == std::string_view::npos) ? std::string_view()
                                           : rest.substr(0, end + 1);
    if (!rest.empty() && rest.back() == '#') {
      size_t hash_start = rest.find_last_not_of('#');
      if (hash_start == std::string_view::npos) {
        rest = std::string_view();
      } else if (rest[hash_start] == ' ' || rest[hash_start] == '\t') {
        rest = rest.substr(0, hash_start);
        end = rest.find_last_not_of(" \t");
        rest = (end == std::string_view::npos) ? std::string_view()
                                               : rest.substr(0, end + 1);
      }
    }
    std::pmr::string content(rest);

    Advance();

    blocks.emplace_back(std::in_place_type<Heading>, level, std::move(content));
    return true;
  }

  bool TryParseFencedCodeBlock(std::pmr::vector<BlockNode>& blocks) {
    std::string_view line = CurrentLine();
    int indent = detail::CountIndent(line);
    if (indent >= 4) [[unlikely]]
      return false;

    auto trimmed = detail::TrimLeft(line);
    if (trimmed.empty()) [[unlikely]]
      return false;

    char fence_char = trimmed[0];
    if (fence_char != '`' && fence_char != '~') [[unlikely]]
      return false;

    size_t fence_length = 0;
    while (fence_length < trimmed.size() &&
           trimmed[fence_length] == fence_char) {
      ++fence_length;
    }

    if (fence_length < 3) [[unlikely]]
      return false;

    // Check for backtick in info string (not allowed for backtick fences)
    std::pmr::string info_string(detail::Trim(trimmed.substr(fence_length)));
    if (fence_char == '`' && info_string.find('`') != std::string::npos)
        [[unlikely]] {
      return false;
    }
    // Decode backslash escapes in info string
    info_string = detail::DecodeEscapesAndEntities(info_string);

    Advance();

    // Collect code content
    std::pmr::string content;
    bool found_closing = false;
    while (!AtEnd()) {
      std::string_view code_line = CurrentLine();

      // Check for closing fence - must be indented < 4 spaces
      int close_indent = detail::CountIndent(code_line);
      if (close_indent < 4) {
        auto code_trimmed = detail::TrimLeft(code_line);

        // Count fence characters at start
        size_t close_fence_len = 0;
        while (close_fence_len < code_trimmed.size() &&
               code_trimmed[close_fence_len] == fence_char) {
          ++close_fence_len;
        }

        // Closing fence must be at least as long as opening fence
        if (close_fence_len >= fence_length) {
          // Rest of line must be only whitespace
          bool is_closing = true;
          for (size_t j = close_fence_len; j < code_trimmed.size(); ++j) {
            if (code_trimmed[j] != ' ' && code_trimmed[j] != '\t') {
              is_closing = false;
              break;
            }
          }
          if (is_closing) {
            Advance();
            found_closing = true;
            break;
          }
        }
      }

      // Remove indentation from code line (appends directly, no intermediate
      // string)
      detail::AppendRemoveIndent(code_line, indent, content);
      content += '\n';
      Advance();
    }

    // If unclosed, remove the trailing newline that was added for the last line
    // (CommonMark spec: unclosed blocks don't include final newline)
    if (!found_closing && !content.empty() && content.back() == '\n') {
      content.pop_back();
    }

    last_fence_found_closing_ = found_closing;
    blocks.emplace_back(std::in_place_type<CodeBlock>, std::move(info_string),
                        std::move(content), true, fence_char);
    return true;
  }

  // Parse a GitHub-style <details> section that starts on the current line.
  // Unlike other type-6 HTML blocks, blank lines do not terminate the block:
  // lines are consumed until a line beginning with the closing </details> tag
  // (case-insensitive) or end of input. Emits a single DetailsBlock.
  bool TryParseDetailsBlock(std::pmr::vector<BlockNode>& blocks) {
    std::pmr::string raw;
    bool closed = false;
    while (!AtEnd()) {
      std::string_view line = CurrentLine();
      auto trimmed = detail::TrimLeft(line);
      if (detail::StartsWithInsensitive(trimmed, "</details")) {
        closed = true;
        Advance();
        break;
      }
      raw += line;
      raw += '\n';
      Advance();
    }

    // A closing tag on the same line as the opening tag closes the section.
    if (!closed) {
      size_t close = detail::FindInsensitive(raw, "</details");
      if (close != std::string_view::npos) {
        raw.erase(close);
        closed = true;
      }
    }

    // Drop the trailing newline of the final line.
    if (!raw.empty() && raw.back() == '\n') raw.pop_back();

    // Attributes of the opening <details ...> tag (up to its first '>').
    std::pmr::vector<std::pair<std::pmr::string, std::pmr::string>> attributes;
    size_t details_tag_end = raw.find('>');
    if (details_tag_end != std::string_view::npos)
      attributes = detail::ParseTagAttributes(
          std::string_view(raw.data(), details_tag_end + 1));

    // Locate the optional <summary> ... </summary> child.
    std::pmr::string summary_raw;
    size_t content_start = 0;
    size_t summary_open = detail::FindInsensitive(raw, "<summary");
    if (summary_open != std::string_view::npos) {
      size_t tag_end = raw.find('>', summary_open);
      if (tag_end != std::string_view::npos) {
        size_t summary_close =
            detail::FindInsensitive(raw.substr(tag_end), "</summary>");
        if (summary_close != std::string_view::npos) {
          // Absolute range of the summary text: after the '>' of the opening
          // <summary ...> tag up to the start of the closing </summary> tag.
          summary_raw.assign(raw.substr(tag_end + 1, summary_close - 1));
          summary_raw = std::pmr::string(detail::Trim(summary_raw));
          content_start = tag_end + summary_close +
                          std::string_view("</summary>").size();
        }
      }
    } else {
      // No <summary> child: content starts after the opening <details ...> tag.
      size_t tag_end = raw.find('>');
      if (tag_end != std::string_view::npos) content_start = tag_end + 1;
    }

    if (content_start == 0) {
      // The opening <details ...> tag was never terminated by '>' (e.g. a
      // partially streamed tag). Recursing into ParseBlocks with the same
      // input would never make progress and overflow the stack, so fall back
      // to a plain (unclosed) HTML block.
      last_html_end_condition_found_ = false;
      blocks.emplace_back(std::in_place_type<HtmlBlock>, std::move(raw), 6);
      return true;
    }

    // Parse the optional <summary> content as regular markdown so its block
    // structure (headings, emphasis, ...) is preserved, like the section body.
    std::pmr::vector<BlockNodeId> summary;
    if (!summary_raw.empty()) {
      std::pmr::vector<BlockNode> summary_blocks;
      ParseNestedInto(summary_raw, summary_blocks);
      for (auto& node : summary_blocks) {
        summary.push_back(doc_->AddBlock(std::move(node)));
      }
    }

    std::pmr::string content;
    if (content_start < raw.size()) {
      content.assign(raw.substr(content_start));
    }

    // Parse the section body as regular markdown so its block structure
    // (paragraphs, code blocks, lists, ...) is preserved.
    std::pmr::vector<BlockNodeId> children;
    if (!content.empty()) {
      std::pmr::vector<BlockNode> body_blocks;
      ParseNestedInto(content, body_blocks);
      for (auto& node : body_blocks) {
        children.push_back(doc_->AddBlock(std::move(node)));
      }
    }

    blocks.emplace_back(std::in_place_type<DetailsBlock>, std::move(summary),
                        std::move(children), closed, std::move(attributes));
    return true;
  }

  bool TryParseHtmlBlock(std::pmr::vector<BlockNode>& blocks) {
    std::string_view line = CurrentLine();
    int indent = detail::CountIndent(line);
    if (indent >= 4) [[unlikely]]
      return false;

    auto trimmed = detail::TrimLeft(line);
    if (trimmed.empty() || trimmed[0] != '<') [[unlikely]]
      return false;

    auto [block_type, end_condition_sv] = detail::ClassifyHtmlBlock(trimmed);
    if (block_type == 0) [[unlikely]]
      return false;

    // GitHub-style <details> section: consume until the matching </details>
    // line and emit a structured DetailsBlock instead of a raw HtmlBlock.
    if (block_type == 6 &&
        detail::StartsWithInsensitive(trimmed, "<details")) {
      return TryParseDetailsBlock(blocks);
    }

    // Collect HTML block content
    std::pmr::string content;
    bool end_condition_found = false;

    while (!AtEnd()) {
      std::string_view html_line = CurrentLine();
      content += html_line;
      content += '\n';

      // Check for end condition using case-insensitive search (no temp string)
      if (block_type <= 5 &&
          detail::StringContainsInsensitive(html_line, end_condition_sv)) {
        end_condition_found = true;
        Advance();
        break;
      }

      Advance();

      // Types 6 and 7 end at blank line
      if ((block_type == 6 || block_type == 7) &&
          (AtEnd() || detail::IsBlankLine(CurrentLine()))) {
        break;
      }
    }

    last_html_end_condition_found_ = end_condition_found;
    blocks.emplace_back(std::in_place_type<HtmlBlock>, std::move(content),
                        block_type);
    return true;
  }

  bool TryParseBlockQuote(std::pmr::vector<BlockNode>& blocks) {
    std::string_view line = CurrentLine();
    int indent = detail::CountIndent(line);
    if (indent >= 4) [[unlikely]]
      return false;

    auto trimmed = detail::TrimLeft(line);
    if (trimmed.empty() || trimmed[0] != '>') [[unlikely]]
      return false;

    // Build a single contiguous buffer for nested content
    std::pmr::string nested_buf(Scratch());
    // Reserve a reasonable amount to reduce reallocations
    nested_buf.reserve(256);

    // Track state for lazy continuation rules
    bool last_line_was_blank = false;
    bool in_fenced_code = false;
    char fence_char = 0;
    size_t fence_length = 0;
    bool in_indented_code = false;
    bool in_paragraph = false;

    while (!AtEnd()) {
      std::string_view bq_line = CurrentLine();
      auto bq_trimmed = detail::TrimLeft(bq_line);

      if (bq_trimmed.starts_with(">")) {
        // Append the post-'>' content directly into nested_buf (no
        // intermediate copy) and derive a non-owning view for the checks.
        // The trailing newline is appended after the checks so that the view
        // stays valid for them.
        size_t content_start = nested_buf.size();
        detail::AppendBlockQuoteContent(bq_line, nested_buf);
        std::string_view inner(nested_buf.data() + content_start,
                               nested_buf.size() - content_start);

        // Track if this line is blank inside the blockquote
        auto inner_trimmed = detail::TrimLeft(inner);
        last_line_was_blank = inner_trimmed.empty();
        int inner_indent = detail::CountIndent(inner);

        // Track block types for lazy continuation rules
        if (!in_fenced_code) {
          if (inner_indent >= 4 && !inner_trimmed.empty()) {
            // Could be indented code block (if not continuing paragraph)
            if (!in_paragraph) {
              in_indented_code = true;
              in_paragraph = false;
            }
          } else if (!inner_trimmed.empty()) {
            // Non-indented, non-blank line starts/continues paragraph
            in_indented_code = false;
            in_paragraph = true;
          }
        }

        if (last_line_was_blank) {
          in_paragraph = false;
          in_indented_code = false;
        }

        // Track fenced code blocks inside the blockquote
        if (!in_fenced_code && !inner_trimmed.empty() &&
            (inner_trimmed[0] == '`' || inner_trimmed[0] == '~')) {
          char potential_fence = inner_trimmed[0];
          size_t potential_len = 0;
          while (potential_len < inner_trimmed.size() &&
                 inner_trimmed[potential_len] == potential_fence) {
            ++potential_len;
          }
          if (potential_len >= 3) {
            if (potential_fence == '~' ||
                inner_trimmed.substr(potential_len).find('`') ==
                    std::string_view::npos) {
              in_fenced_code = true;
              fence_char = potential_fence;
              fence_length = potential_len;
            }
          }
        } else if (in_fenced_code && !inner_trimmed.empty() &&
                   inner_trimmed[0] == fence_char) {
          size_t close_len = 0;
          while (close_len < inner_trimmed.size() &&
                 inner_trimmed[close_len] == fence_char) {
            ++close_len;
          }
          if (close_len >= fence_length) {
            bool is_close = true;
            for (size_t j = close_len; j < inner_trimmed.size(); ++j) {
              if (inner_trimmed[j] != ' ' && inner_trimmed[j] != '\t') {
                is_close = false;
                break;
              }
            }
            if (is_close) in_fenced_code = false;
          }
        }

        nested_buf += '\n';
        Advance();
      } else if (!bq_trimmed.empty() && !nested_buf.empty()) {
        // Lazy continuation - only for paragraphs, not after blank lines,
        // not inside fenced code blocks
        bool is_continuation = true;
        int bq_indent = detail::CountIndent(bq_line);

        // No lazy continuation after blank line in blockquote
        if (last_line_was_blank) {
          is_continuation = false;
        }

        // No lazy continuation inside fenced code block or indented code block
        if (in_fenced_code || in_indented_code) {
          is_continuation = false;
        }

        // Lazy continuation only continues paragraphs
        if (!in_paragraph) {
          is_continuation = false;
        }

        // Check for block-level interrupts - but only if not indented 4+ spaces
        // (4+ space indented lines can't start block-level elements, so they're
        // always lazy continuation if other conditions are met)
        if (is_continuation && bq_indent < 4) {
          if (bq_trimmed.starts_with("#") || bq_trimmed.starts_with("```") ||
              bq_trimmed.starts_with("~~~") || bq_trimmed.starts_with("---") ||
              bq_trimmed.starts_with("***") || bq_trimmed.starts_with("___") ||
              bq_trimmed.starts_with("- ") || bq_trimmed.starts_with("* ") ||
              bq_trimmed.starts_with("+ ") || bq_trimmed.starts_with(">") ||
              (bq_trimmed.size() >= 2 &&
               detail::IsDigit(static_cast<unsigned char>(bq_trimmed[0])))) {
            is_continuation = false;
          }
        }

        if (is_continuation) {
          // Mark lazy continuation with special prefix to prevent setext
          // heading (but don't add if already marked)
          if (!bq_trimmed.empty() && bq_trimmed[0] == '\x01') {
            nested_buf.append(bq_trimmed.data(), bq_trimmed.size());
          } else {
            nested_buf += '\x01';
            nested_buf.append(bq_trimmed.data(), bq_trimmed.size());
          }
          nested_buf += '\n';
          last_line_was_blank = false;
          // Lazy continuation continues the paragraph
          in_paragraph = true;
          in_indented_code = false;
          Advance();
        } else {
          break;
        }
      } else {
        break;
      }
    }

    if (nested_buf.empty()) [[unlikely]]
      return false;

    std::pmr::vector<BlockNode> nested_blocks(Scratch());
    ParseNestedInto(nested_buf, nested_blocks);

    BlockQuote bq;
    for (auto& node : nested_blocks) {
      bq.children.push_back(doc_->AddBlock(std::move(node)));
    }
    blocks.emplace_back(std::in_place_type<BlockQuote>, std::move(bq.children));
    return true;
  }

  bool TryParseList(std::pmr::vector<BlockNode>& blocks) {
    std::string_view line = CurrentLine();
    int indent = detail::CountIndent(line);
    if (indent >= 4) [[unlikely]]
      return false;

    auto trimmed = detail::TrimLeft(line);
    if (trimmed.empty()) [[unlikely]]
      return false;

    // Check for bullet list
    bool is_ordered = false;
    int start = 1;
    char delimiter = '.';
    char bullet_char = '-';

    if (trimmed[0] == '-' || trimmed[0] == '+' || trimmed[0] == '*') {
      bullet_char = trimmed[0];
      // Marker can be followed by space, or be at end of line (empty item)
      if (trimmed.size() == 1) {
        // Just the bullet - valid empty item
      } else if (trimmed[1] == ' ' || trimmed[1] == '\t') {
        // Bullet + space - valid
      } else {
        return false;  // Not a list marker
      }
    } else if (detail::IsDigit(static_cast<unsigned char>(trimmed[0]))) {
      // Ordered list
      is_ordered = true;
      size_t num_end = 0;
      while (num_end < trimmed.size() &&
             detail::IsDigit(static_cast<unsigned char>(trimmed[num_end]))) {
        ++num_end;
      }
      if (num_end == 0 || num_end > 9) [[unlikely]]
        return false;
      if (num_end >= trimmed.size()) [[unlikely]]
        return false;

      char delim = trimmed[num_end];
      if (delim != '.' && delim != ')') [[unlikely]]
        return false;

      uint32_t start_val;
      if (!ParseUint(trimmed.substr(0, num_end), start_val, 10)) return false;
      start = static_cast<int>(start_val);
      delimiter = delim;

      // Marker can be followed by space, or be at end of line (empty item)
      if (num_end + 1 >= trimmed.size()) {
        // Just number + delimiter - valid empty item
      } else if (trimmed[num_end + 1] == ' ' || trimmed[num_end + 1] == '\t') {
        // Number + delimiter + space - valid
      } else {
        return false;  // Not a list marker
      }
    } else {
      return false;
    }

    // Parse list items
    List list;
    list.items.reserve(4);  // Most lists are short
    list.is_ordered = is_ordered;
    list.start = start;
    list.delimiter = delimiter;
    list.bullet_char = bullet_char;
    list.is_tight = true;

    bool had_blank_line = false;

    while (!AtEnd()) {
      std::string_view item_line = CurrentLine();
      int item_indent = detail::CountIndent(item_line);
      auto item_trimmed = detail::TrimLeft(item_line);

      // Check for blank line
      if (detail::IsBlankLine(item_line)) {
        had_blank_line = true;
        Advance();
        continue;
      }

      // Check for thematic break first - it takes precedence over list items
      // Only applies to bullet lists where the bullet could be a thematic break
      if (!is_ordered && item_indent < 4 && !item_trimmed.empty()) {
        char first = item_trimmed[0];
        if (first == '*' || first == '-' || first == '_') {
          int count = 0;
          bool valid_break = true;
          for (char ch : item_trimmed) {
            if (ch == first) {
              ++count;
            } else if (ch != ' ' && ch != '\t') {
              valid_break = false;
              break;
            }
          }
          if (valid_break && count >= 3) {
            // This is a thematic break, not a list item - end the list
            break;
          }
        }
      }

      // Check for new list item
      bool is_new_item = false;
      if (is_ordered) {
        if (item_indent < 4 && !item_trimmed.empty() &&
            detail::IsDigit(static_cast<unsigned char>(item_trimmed[0]))) {
          size_t num_end = 0;
          while (num_end < item_trimmed.size() &&
                 detail::IsDigit(
                     static_cast<unsigned char>(item_trimmed[num_end]))) {
            ++num_end;
          }
          if (num_end > 0 && num_end <= 9 && num_end < item_trimmed.size() &&
              item_trimmed[num_end] == delimiter) {
            if (num_end + 1 >= item_trimmed.size() ||
                item_trimmed[num_end + 1] == ' ' ||
                item_trimmed[num_end + 1] == '\t') {
              is_new_item = true;
            }
          }
        }
      } else {
        if (item_indent < 4 && !item_trimmed.empty() &&
            item_trimmed[0] == bullet_char) {
          // Match if: just the bullet, or bullet followed by space/tab
          if (item_trimmed.size() == 1 || item_trimmed[1] == ' ' ||
              item_trimmed[1] == '\t') {
            is_new_item = true;
          }
        }
      }

      if (!is_new_item && !list.items.empty()) {
        // Not a new item - the list is complete.
        // Any valid continuation lines should have been collected by the inner
        // loop. If we're here, the line belongs to a different block.
        break;
      }

      if (is_new_item || list.items.empty()) {
        // Start new item
        if (had_blank_line && !list.items.empty()) {
          list.is_tight = false;
        }

        ListItem item;
        // Build a single contiguous buffer for item content
        std::pmr::string item_buf(Scratch());

        // Get first line content with proper tab handling. Only materialize an
        // expanded copy when the line actually contains a tab; otherwise reuse
        // the original (valid) view to avoid a per-line allocation.
        std::pmr::string expanded_storage;
        std::string_view expanded_line;
        if (item_line.find('\t') != std::string_view::npos) {
          expanded_storage = detail::ExpandTabs(item_line);
          expanded_line = expanded_storage;
        } else {
          expanded_line = item_line;
        }
        std::string_view expanded_trimmed = detail::TrimLeft(expanded_line);

        // Calculate required indent based on content start position
        // For `-    foo`, the content starts at column 5, so required_indent is
        // 5 For `-\tfoo`, the content starts at column 4, so required_indent is
        // 4
        int expanded_item_indent = detail::CountIndent(expanded_line);
        int content_start = expanded_item_indent;

        // Find where the marker ends (bullet or number+delimiter)
        size_t marker_end = 0;
        if (is_ordered) {
          while (marker_end < expanded_trimmed.size() &&
                 detail::IsDigit(static_cast<unsigned char>(
                     expanded_trimmed[marker_end]))) {
            ++marker_end;
          }
          marker_end++;  // +1 for delimiter
        } else {
          marker_end = 1;  // bullet
        }

        // Find where content actually starts (skip all spaces after marker)
        size_t content_pos = marker_end;
        while (content_pos < expanded_trimmed.size() &&
               (expanded_trimmed[content_pos] == ' ' ||
                expanded_trimmed[content_pos] == '\t')) {
          ++content_pos;
        }

        // Calculate skip amount for first_content extraction
        size_t skip;

        // Apply the "5-space rule": if first non-blank content is 5+ positions
        // after the marker, it becomes an indented code block. Cap skip
        // at marker + 1, so the extra spaces become code block indent.
        if (content_pos == expanded_trimmed.size()) {
          // Empty item - content start is marker + 1 space
          content_start += static_cast<int>(marker_end) + 1;
          skip = content_pos;  // Skip everything (empty)
        } else if (static_cast<int>(content_pos) >
                   static_cast<int>(marker_end) + 4) {
          // 5-space rule: content starts at marker + 1, making excess into code
          content_start += static_cast<int>(marker_end) + 1;
          skip = marker_end + 1;  // Only skip marker + 1 space
        } else {
          content_start += static_cast<int>(content_pos);
          skip = content_pos;  // Skip to actual content
        }

        std::string_view first_content = expanded_trimmed.substr(skip);

        // GFM `tasklist` extension: if the first line of the item begins with a
        // `[ ]`, `[x]` or `[X]` marker followed by a space or tab, it is a task
        // list item. The marker (plus any following whitespace) is consumed so
        // the remaining text becomes the item's paragraph content, and the
        // checked state is recorded for the renderer. Mirrors cmark-gfm's
        // tasklist extension (marker = `[` + (space|x|X) + `]`, then at least
        // one whitespace before other content).
        if (enable_tasklist && first_content.size() >= 4 &&
            first_content[0] == '[' &&
            (first_content[1] == ' ' || first_content[1] == 'x' ||
             first_content[1] == 'X') &&
            first_content[2] == ']' &&
            (first_content[3] == ' ' || first_content[3] == '\t')) {
          item.is_tasklist = true;
          item.tasklist_checked =
              (first_content[1] == 'x' || first_content[1] == 'X');
          size_t content_after_marker = 3;
          while (content_after_marker < first_content.size() &&
                 (first_content[content_after_marker] == ' ' ||
                  first_content[content_after_marker] == '\t')) {
            ++content_after_marker;
          }
          first_content.remove_prefix(content_after_marker);
        }

        item_buf += first_content;
        item_buf += '\n';
        Advance();
        had_blank_line = false;

        // Collect continuation lines
        int required_indent = content_start;
        bool first_line_empty =
            first_content.empty() || detail::IsBlankLine(first_content);

        // Track fenced code blocks to ignore blank lines within them
        bool in_item_fenced_code = false;
        char item_fence_char = 0;
        size_t item_fence_length = 0;

        // Track if we've seen a nested list marker (indent 1-3)
        // This helps distinguish nested content from direct code blocks
        bool has_nested_list = false;

        // Check if first line starts a fenced code block
        auto first_trimmed = detail::TrimLeft(first_content);
        if (!first_trimmed.empty() &&
            (first_trimmed[0] == '`' || first_trimmed[0] == '~')) {
          size_t fence_len = 0;
          while (fence_len < first_trimmed.size() &&
                 first_trimmed[fence_len] == first_trimmed[0]) {
            ++fence_len;
          }
          if (fence_len >= 3) {
            in_item_fenced_code = true;
            item_fence_char = first_trimmed[0];
            item_fence_length = fence_len;
          }
        }

        while (!AtEnd()) {
          std::string_view cont_line = CurrentLine();

          if (detail::IsBlankLine(cont_line)) {
            // If the first line was empty (just the marker), a blank line
            // ends this item - we can't continue after a blank for empty items
            if (first_line_empty) {
              Advance();
              had_blank_line = true;
              break;  // End item - don't collect more lines
            }
            item_buf += '\n';
            // Only count blank lines outside fenced code blocks
            if (!in_item_fenced_code) {
              had_blank_line = true;
            }
            Advance();
            continue;
          }

          // Expand tabs for proper indent calculation. Reuse the original view
          // when there is no tab to avoid a per-line allocation.
          std::pmr::string cont_storage;
          std::string_view expanded_cont;
          if (cont_line.find('\t') != std::string_view::npos) {
            cont_storage = detail::ExpandTabs(cont_line);
            expanded_cont = cont_storage;
          } else {
            expanded_cont = cont_line;
          }
          int cont_indent = detail::CountIndent(expanded_cont);
          auto cont_trimmed = detail::TrimLeft(expanded_cont);

          // Check for new list item (same type) - only at original list indent
          // If indented to be content of current item, it should be parsed as
          // nested list by the BlockParser, not as a sibling item here
          bool is_another_item = false;
          if (cont_indent < required_indent && cont_indent < 4) {
            if (is_ordered && !cont_trimmed.empty() &&
                detail::IsDigit(static_cast<unsigned char>(cont_trimmed[0]))) {
              size_t num_end = 0;
              while (num_end < cont_trimmed.size() &&
                     detail::IsDigit(
                         static_cast<unsigned char>(cont_trimmed[num_end]))) {
                ++num_end;
              }
              if (num_end > 0 && num_end < cont_trimmed.size() &&
                  cont_trimmed[num_end] == delimiter) {
                is_another_item = true;
              }
            } else if (!is_ordered && !cont_trimmed.empty() &&
                       cont_trimmed[0] == bullet_char) {
              // Match if: just the bullet, or bullet followed by space/tab
              if (cont_trimmed.size() == 1 || cont_trimmed[1] == ' ' ||
                  cont_trimmed[1] == '\t') {
                is_another_item = true;
              }
            }
          }

          if (is_another_item) {
            break;
          }

          // Check for other block-level interrupts
          if (cont_indent < required_indent) {
            // Check for different list type (different bullet or delimiter)
            // A different list type starts a new list
            // List markers at indent < 4 end the current item (sibling level)
            // But at indent >= 4, they can be lazy continuation (not siblings)
            if (cont_indent < 4) {
              if (!cont_trimmed.empty() &&
                  (cont_trimmed[0] == '-' || cont_trimmed[0] == '+' ||
                   cont_trimmed[0] == '*')) {
                // Bullet list marker
                if (cont_trimmed.size() == 1 || cont_trimmed[1] == ' ' ||
                    cont_trimmed[1] == '\t') {
                  break;  // List marker ends item
                }
              }
              if (!cont_trimmed.empty() &&
                  detail::IsDigit(
                      static_cast<unsigned char>(cont_trimmed[0]))) {
                size_t ne = 0;
                while (ne < cont_trimmed.size() &&
                       detail::IsDigit(
                           static_cast<unsigned char>(cont_trimmed[ne]))) {
                  ++ne;
                }
                if (ne > 0 && ne < cont_trimmed.size() &&
                    (cont_trimmed[ne] == '.' || cont_trimmed[ne] == ')')) {
                  break;  // List marker ends item
                }
              }
            }

            // Check for block elements that can interrupt
            if (cont_trimmed.starts_with(">") ||
                cont_trimmed.starts_with("#") ||
                cont_trimmed.starts_with("```") ||
                cont_trimmed.starts_with("~~~")) {
              break;
            }
            // Check for thematic break
            if (cont_trimmed.size() >= 3) {
              char first = cont_trimmed[0];
              if (first == '-' || first == '*' || first == '_') {
                int count = 0;
                bool valid_break = true;
                for (char ch : cont_trimmed) {
                  if (ch == first) {
                    ++count;
                  } else if (ch != ' ') {
                    valid_break = false;
                    break;
                  }
                }
                if (valid_break && count >= 3) {
                  break;
                }
              }
            }
            // End list if not properly indented
            if (!had_blank_line) {
              // Lazy continuation - mark with \x01 to prevent block parsing
              // (but don't add if already marked)
              if (!cont_trimmed.empty() && cont_trimmed[0] == '\x01') {
                item_buf.append(cont_trimmed.data(), cont_trimmed.size());
              } else {
                item_buf += '\x01';
                item_buf.append(cont_trimmed.data(), cont_trimmed.size());
              }
              item_buf += '\n';
              Advance();
            } else {
              break;
            }
          } else {
            // Properly indented continuation - remove required_indent columns
            // directly into item_buf (no intermediate allocation) and derive a
            // non-owning view for the checks. The view stays valid because
            // item_buf is not mutated until the trailing newline is appended
            // after the checks below.
            size_t dedented_start = item_buf.size();
            detail::AppendRemoveIndent(expanded_cont, required_indent,
                                       item_buf);
            std::string_view dedented(item_buf.data() + dedented_start,
                                      item_buf.size() - dedented_start);

            // Track fenced code blocks
            auto dedented_trimmed = detail::TrimLeft(dedented);
            int dedented_indent = detail::CountIndent(dedented);
            if (!in_item_fenced_code && dedented_indent < 4 &&
                !dedented_trimmed.empty() &&
                (dedented_trimmed[0] == '`' || dedented_trimmed[0] == '~')) {
              size_t fence_len = 0;
              while (fence_len < dedented_trimmed.size() &&
                     dedented_trimmed[fence_len] == dedented_trimmed[0]) {
                ++fence_len;
              }
              if (fence_len >= 3) {
                in_item_fenced_code = true;
                item_fence_char = dedented_trimmed[0];
                item_fence_length = fence_len;
              }
            } else if (in_item_fenced_code && !dedented_trimmed.empty() &&
                       dedented_trimmed[0] == item_fence_char) {
              size_t close_len = 0;
              while (close_len < dedented_trimmed.size() &&
                     dedented_trimmed[close_len] == item_fence_char) {
                ++close_len;
              }
              if (close_len >= item_fence_length) {
                bool is_close = true;
                for (size_t j = close_len; j < dedented_trimmed.size(); ++j) {
                  if (dedented_trimmed[j] != ' ' &&
                      dedented_trimmed[j] != '\t') {
                    is_close = false;
                    break;
                  }
                }
                if (is_close) in_item_fenced_code = false;
              }
            }

            // Check for nested list markers (indent 0-3 with list marker)
            if (!has_nested_list && dedented_indent >= 0 &&
                dedented_indent < 4 && !dedented_trimmed.empty()) {
              char fc = dedented_trimmed[0];
              if (fc == '-' || fc == '+' || fc == '*') {
                if (dedented_trimmed.size() == 1 ||
                    dedented_trimmed[1] == ' ' || dedented_trimmed[1] == '\t') {
                  has_nested_list = true;
                }
              } else if (detail::IsDigit(static_cast<unsigned char>(fc))) {
                size_t ne = 0;
                while (ne < dedented_trimmed.size() &&
                       detail::IsDigit(
                           static_cast<unsigned char>(dedented_trimmed[ne]))) {
                  ++ne;
                }
                if (ne > 0 && ne < dedented_trimmed.size() &&
                    (dedented_trimmed[ne] == '.' ||
                     dedented_trimmed[ne] == ')')) {
                  has_nested_list = true;
                }
              }
            }

            // If there was a blank line before this content, list becomes loose
            // ONLY if the content is at the item's direct level:
            // - indent 0: direct paragraph
            // - indent >= 4 AND no nested list: direct indented code block
            if (had_blank_line) {
              if (dedented_indent == 0 ||
                  (dedented_indent >= 4 && !has_nested_list)) {
                list.is_tight = false;
              }
            }
            item_buf += '\n';
            Advance();
            had_blank_line = false;
          }
        }

        // Remove trailing blank lines (pop_back removed empty strings =
        // trailing \n)
        while (item_buf.size() >= 2 && item_buf.back() == '\n') {
          // Check if the preceding line is empty (i.e., this is a trailing
          // blank line) A trailing blank line in our buffer is just a bare \n
          // after another \n or at the end. Strip it.
          item_buf.pop_back();
        }

        // Parse item content
        if (!item_buf.empty()) {
          std::pmr::vector<BlockNode> item_blocks(Scratch());
          ParseNestedInto(item_buf, item_blocks);

          item.children.reserve(item_blocks.size());
          for (auto& node : item_blocks) {
            item.children.push_back(doc_->AddBlock(std::move(node)));
          }
        }

        list.items.push_back(std::move(item));
      }
    }

    // Return false if no items were collected
    if (list.items.empty()) {
      return false;
    }

    // Check for loose list (items separated by blank lines)
    for (auto& item : list.items) {
      item.is_tight = list.is_tight;
    }

    blocks.emplace_back(std::in_place_type<List>, is_ordered, start, delimiter,
                        bullet_char, list.is_tight, std::move(list.items));
    return true;
  }

  bool TryParseIndentedCodeBlock(std::pmr::vector<BlockNode>& blocks) {
    std::string_view line = CurrentLine();
    int indent = detail::CountIndent(line);
    if (indent < 4) [[unlikely]]
      return false;

    std::pmr::string content;

    while (!AtEnd()) {
      std::string_view code_line = CurrentLine();
      int code_indent = detail::CountIndent(code_line);

      if (detail::IsBlankLine(code_line)) {
        // Blank lines within code blocks preserve whitespace beyond 4 spaces
        if (code_indent >= 4) {
          detail::AppendRemoveIndent(code_line, 4, content);
        }
        content += '\n';
        Advance();
        continue;
      }

      if (code_indent < 4) break;

      detail::AppendRemoveIndent(code_line, 4, content);
      content += '\n';
      Advance();
    }

    // Remove trailing blank lines
    while (!content.empty() && content.back() == '\n') {
      size_t last_newline = content.find_last_not_of('\n');
      if (last_newline == std::string::npos) {
        content.clear();
      } else {
        content = content.substr(0, last_newline + 1);
        content += '\n';
        break;
      }
    }

    if (content.empty()) [[unlikely]]
      return false;

    blocks.emplace_back(std::in_place_type<CodeBlock>, std::pmr::string{},
                        std::move(content), false);
    return true;
  }

  // If `header_line` (the current line) is a table header and the following
  // line is a matching delimiter row, consume the whole table (header,
  // delimiter and body rows) and fill `out`. Returns false without consuming
  // anything when it is not a table.
  bool TryBuildTable(std::string_view header_line, Table* out) {
    // The header row is the current line at call time; remember it so the
    // Table (which may be pushed after a leading Paragraph) reports its real
    // start line.
    last_table_start_line_ = line_idx_;
    if (line_idx_ + 1 >= lines_.size()) return false;
    std::string_view next_raw = lines_[line_idx_ + 1];
    std::string_view next_line = detail::TrimLeft(next_raw);

    // A setext underline following a one-line paragraph is a heading, not a
    // single-column table.
    if (detail::IsSetextUnderline(next_raw)) return false;

    // A delimiter row that looks like a list bullet (e.g. `- | -`) is parsed
    // as a list, not a table (lists take precedence).
    if (next_line.size() >= 2 && next_line[0] == '-' &&
        (next_line[1] == ' ' || next_line[1] == '\t')) {
      return false;
    }

    auto header_cells = detail::SplitTableRow(header_line);
    if (header_cells.empty()) return false;

    auto delim_cells = detail::SplitTableRow(next_line);
    if (delim_cells.empty() || delim_cells.size() != header_cells.size())
      return false;

    std::pmr::vector<TableAlign> aligns;
    aligns.reserve(delim_cells.size());
    for (auto& c : delim_cells) {
      TableAlign a;
      if (!detail::ClassifyDelimiterCell(c, a)) return false;
      aligns.push_back(a);
    }

    const int ncols = static_cast<int>(header_cells.size());
    out->alignments = std::move(aligns);

    TableRow header_row;
    header_row.is_header = true;
    header_row.cells.reserve(ncols);
    for (int i = 0; i < ncols; ++i) {
      header_row.cells.emplace_back(std::pmr::string(header_cells[i]));
    }
    out->rows.push_back(std::move(header_row));

    // Consume the header and delimiter lines.
    Advance();
    Advance();

    // Body rows: consume while the line continues the table. Short rows are
    // padded with empty cells up to kMaxTableAutoCompletedCells in total.
    size_t auto_completed_cells = 0;
    while (!AtEnd()) {
      std::string_view body_line = CurrentLine();
      if (!detail::IsTableRowLine(body_line)) break;
      auto body_cells = detail::SplitTableRow(detail::TrimLeft(body_line));
      if (body_cells.size() < static_cast<size_t>(ncols)) {
        auto_completed_cells += static_cast<size_t>(ncols) - body_cells.size();
        if (auto_completed_cells > detail::kMaxTableAutoCompletedCells) break;
      }
      TableRow body_row;
      body_row.is_header = false;
      body_row.cells.reserve(ncols);
      for (int i = 0; i < ncols; ++i) {
        if (i < static_cast<int>(body_cells.size())) {
          body_row.cells.emplace_back(std::pmr::string(body_cells[i]));
        } else {
          body_row.cells.emplace_back(std::pmr::string());
        }
      }
      out->rows.push_back(std::move(body_row));
      Advance();
    }

    return true;
  }

  // Join `lines` with '\n' into a single string sized up front.
  static std::pmr::string JoinLines(
      const std::pmr::vector<std::string_view>& lines,
      std::pmr::memory_resource* resource) {
    size_t total = lines.empty() ? 0 : lines.size() - 1;
    for (std::string_view l : lines) total += l.size();
    std::pmr::string out(resource);
    out.reserve(total);
    for (size_t j = 0; j < lines.size(); ++j) {
      if (j > 0) out += '\n';
      out += lines[j];
    }
    return out;
  }

  void ParseParagraph(std::pmr::vector<BlockNode>& blocks) {
    // Store non-owning views into the LineBuffer (which stays valid for the
    // duration of this call) instead of copying each line into its own
    // string. The single join into raw_content at the end is the only copy.
    // The vector is a member so its capacity is reused across paragraphs
    // (ParseParagraph never re-enters itself on the same parser).
    std::pmr::vector<std::string_view>& para_lines = para_lines_;
    para_lines.clear();

    while (!AtEnd()) {
      std::string_view line = CurrentLine();

      if (detail::IsBlankLine(line)) break;

      int indent = detail::CountIndent(line);
      auto trimmed = detail::TrimLeft(line);

      // Check for setext heading underline
      // Skip lines marked with \x01 (lazy continuation in blockquotes)
      if (!para_lines.empty() && indent < 4 && !line.starts_with("\x01")) {
        if (!trimmed.empty() && (trimmed[0] == '=' || trimmed[0] == '-')) {
          char underline_char = trimmed[0];
          // Find where underline chars end
          size_t underline_end = 0;
          while (underline_end < trimmed.size() &&
                 trimmed[underline_end] == underline_char) {
            ++underline_end;
          }
          // Must have at least one underline char
          if (underline_end > 0) {
            // Rest must be only whitespace (no mixed chars like "= =")
            bool valid_underline = true;
            for (size_t j = underline_end; j < trimmed.size(); ++j) {
              if (trimmed[j] != ' ' && trimmed[j] != '\t') {
                valid_underline = false;
                break;
              }
            }
            if (valid_underline) {
              Advance();

              Heading heading;
              heading.level = (underline_char == '=') ? 1 : 2;

              SetRawContent(heading.raw_content, para_lines);
              blocks.push_back(std::move(heading));
              return;
            }
          }
        }
      }

      // Check for block-level interrupts
      // Skip if line is marked with \x01 (lazy continuation)
      if (indent < 4 && !line.starts_with("\x01")) {
        // ATX heading: must be #, ##, etc. followed by space or end of line
        if (trimmed.starts_with("#")) {
          size_t hash_count = 0;
          while (hash_count < trimmed.size() && trimmed[hash_count] == '#') {
            ++hash_count;
          }
          if (hash_count <= 6 &&
              (hash_count >= trimmed.size() || trimmed[hash_count] == ' ' ||
               trimmed[hash_count] == '\t')) {
            break;
          }
        }
        if (trimmed.starts_with(">")) {
          break;
        }
        // Fenced code block: must be at least 3 fence chars, and for backticks,
        // the info string cannot contain backticks
        if (trimmed.size() >= 3 && (trimmed[0] == '`' || trimmed[0] == '~')) {
          char fence_char = trimmed[0];
          size_t fence_len = 0;
          while (fence_len < trimmed.size() &&
                 trimmed[fence_len] == fence_char) {
            ++fence_len;
          }
          if (fence_len >= 3) {
            // For backticks, check that info string doesn't contain backticks
            if (fence_char == '~') {
              break;
            }
            auto info = detail::Trim(trimmed.substr(fence_len));
            if (info.find('`') == std::string_view::npos) {
              break;
            }
          }
        }

        // Check for thematic break
        if (trimmed.size() >= 3) {
          char first = trimmed[0];
          if (first == '-' || first == '*' || first == '_') {
            int count = 0;
            bool valid_break = true;
            for (char ch : trimmed) {
              if (ch == first) {
                ++count;
              } else if (ch != ' ' && ch != '\t') {
                valid_break = false;
                break;
              }
            }
            if (valid_break && count >= 3) {
              break;
            }
          }
        }

        // Check for list item (but not if just -)
        if ((trimmed.starts_with("- ") || trimmed.starts_with("+ ") ||
             trimmed.starts_with("* ")) ||
            (trimmed.size() >= 2 &&
             detail::IsDigit(static_cast<unsigned char>(trimmed[0])))) {
          if (trimmed.starts_with("- ") || trimmed.starts_with("+ ") ||
              trimmed.starts_with("* ")) {
            break;
          }
          // Check for ordered list
          size_t num_end = 0;
          while (
              num_end < trimmed.size() &&
              detail::IsDigit(static_cast<unsigned char>(trimmed[num_end]))) {
            ++num_end;
          }
          if (num_end > 0 && num_end < trimmed.size() &&
              (trimmed[num_end] == '.' || trimmed[num_end] == ')') &&
              num_end + 1 < trimmed.size() &&  // Must have content after marker
              (trimmed[num_end + 1] == ' ' || trimmed[num_end + 1] == '\t')) {
            // Only interrupt if starts with 1
            if (trimmed.substr(0, num_end) == "1") {
              break;
            }
          }
        }

        // Check for HTML block (types 1-6 can interrupt paragraphs)
        if (trimmed.starts_with("<")) {
          // Type 1: script, pre, style, textarea
          static constexpr std::array type1_tags = {
              std::string_view("<script"), std::string_view("<pre"),
              std::string_view("<style"), std::string_view("<textarea")};
          for (auto tag : type1_tags) {
            if (detail::StartsWithInsensitive(trimmed, tag) &&
                (trimmed.size() == tag.size() || trimmed[tag.size()] == ' ' ||
                 trimmed[tag.size()] == '>' || trimmed[tag.size()] == '\t')) {
              goto html_interrupt;
            }
          }
          // Type 2-5
          if (trimmed.starts_with("<!--") || trimmed.starts_with("<?") ||
              trimmed.starts_with("<![CDATA[")) {
            goto html_interrupt;
          }
          if (detail::StartsWithInsensitive(trimmed, "<!doctype")) {
            goto html_interrupt;
          }
          // Type 6: block-level tags
          static constexpr std::array type6_tags = {
              std::string_view("address"),  std::string_view("article"),
              std::string_view("aside"),    std::string_view("base"),
              std::string_view("basefont"), std::string_view("blockquote"),
              std::string_view("body"),     std::string_view("caption"),
              std::string_view("center"),   std::string_view("col"),
              std::string_view("colgroup"), std::string_view("dd"),
              std::string_view("details"),  std::string_view("dialog"),
              std::string_view("dir"),      std::string_view("div"),
              std::string_view("dl"),       std::string_view("dt"),
              std::string_view("fieldset"), std::string_view("figcaption"),
              std::string_view("figure"),   std::string_view("footer"),
              std::string_view("form"),     std::string_view("frame"),
              std::string_view("frameset"), std::string_view("h1"),
              std::string_view("h2"),       std::string_view("h3"),
              std::string_view("h4"),       std::string_view("h5"),
              std::string_view("h6"),       std::string_view("head"),
              std::string_view("header"),   std::string_view("hr"),
              std::string_view("html"),     std::string_view("iframe"),
              std::string_view("legend"),   std::string_view("li"),
              std::string_view("link"),     std::string_view("main"),
              std::string_view("menu"),     std::string_view("menuitem"),
              std::string_view("nav"),      std::string_view("noframes"),
              std::string_view("ol"),       std::string_view("optgroup"),
              std::string_view("option"),   std::string_view("p"),
              std::string_view("param"),    std::string_view("search"),
              std::string_view("section"),  std::string_view("summary"),
              std::string_view("table"),    std::string_view("tbody"),
              std::string_view("td"),       std::string_view("tfoot"),
              std::string_view("th"),       std::string_view("thead"),
              std::string_view("title"),    std::string_view("tr"),
              std::string_view("track"),    std::string_view("ul")};
          bool is_closing = (trimmed.size() >= 2 && trimmed[1] == '/');
          size_t tag_start = is_closing ? 2 : 1;
          size_t tag_end = tag_start;
          while (
              tag_end < trimmed.size() &&
              (detail::IsAlnum(static_cast<unsigned char>(trimmed[tag_end])) ||
               trimmed[tag_end] == '-')) {
            ++tag_end;
          }
          if (tag_end > tag_start) {
            std::string_view tag_name_sv =
                trimmed.substr(tag_start, tag_end - tag_start);
            for (auto t : type6_tags) {
              if (detail::StartsWithInsensitive(tag_name_sv, t) &&
                  tag_name_sv.size() == t.size()) {
                if (tag_end >= trimmed.size() || trimmed[tag_end] == ' ' ||
                    trimmed[tag_end] == '>' || trimmed[tag_end] == '\t' ||
                    trimmed[tag_end] == '/') {
                  goto html_interrupt;
                }
              }
            }
          }
        }
        if (false) {
        html_interrupt:
          break;
        }
      }

      // GFM tables: the current (non-blank, non-interrupting) line is a table
      // header if the following line is a matching delimiter row. Any lines
      // collected so far become a separate preceding paragraph.
      if (enable_tables && indent < 4 &&
          (!trimmed.empty() && trimmed[0] != '\x01')) {
        Table table;
        if (TryBuildTable(trimmed, &table)) {
          if (!para_lines.empty()) {
            Paragraph leading;
            SetRawContent(leading.raw_content, para_lines);
            blocks.push_back(std::move(leading));
          }
          blocks.push_back(std::move(table));
          return;
        }
      }

      // Strip \x01 marker used for lazy continuation lines
      if (!trimmed.empty() && trimmed[0] == '\x01') {
        para_lines.push_back(trimmed.substr(1));
      } else {
        para_lines.push_back(trimmed);
      }
      Advance();
    }

    Paragraph para;
    SetRawContent(para.raw_content, para_lines);
    blocks.push_back(std::move(para));
  }

  void ParseInlines(std::pmr::vector<BlockNode>& blocks, InlineParser& parser) {
    for (auto& block : blocks) {
      std::visit(
          [&parser, this](auto&& node) {
            using T = std::decay_t<decltype(node)>;
            if constexpr (std::is_same_v<T, Paragraph>) {
              node.children =
                  parser.Parse(StableText(node.raw_content, parser));
            } else if constexpr (std::is_same_v<T, Heading>) {
              node.children =
                  parser.Parse(StableText(node.raw_content, parser));
            } else if constexpr (std::is_same_v<T, DetailsBlock>) {
              ParseInlines(node.summary, parser);
              ParseInlines(node.children, parser);
            } else if constexpr (std::is_same_v<T, BlockQuote>) {
              ParseInlines(node.children, parser);
            } else if constexpr (std::is_same_v<T, List>) {
              for (auto& item : node.items) {
                ParseInlines(item.children, parser);
              }
            } else if constexpr (std::is_same_v<T, Table>) {
              for (auto& row : node.rows) {
                for (auto& cell : row.cells) {
                  cell.children =
                      parser.Parse(StableText(cell.raw_content, parser));
                }
              }
            }
          },
          block);
    }
  }

  void ParseInlines(const std::pmr::vector<BlockNodeId>& block_ids,
                    InlineParser& parser) {
    for (BlockNodeId id : block_ids) {
      auto& block = doc_->block_nodes[id];
      std::visit(
          [&parser, this](auto&& node) {
            using T = std::decay_t<decltype(node)>;
            if constexpr (std::is_same_v<T, Paragraph>) {
              node.children =
                  parser.Parse(StableText(node.raw_content, parser));
            } else if constexpr (std::is_same_v<T, Heading>) {
              node.children =
                  parser.Parse(StableText(node.raw_content, parser));
            } else if constexpr (std::is_same_v<T, DetailsBlock>) {
              ParseInlines(node.summary, parser);
              ParseInlines(node.children, parser);
            } else if constexpr (std::is_same_v<T, BlockQuote>) {
              ParseInlines(node.children, parser);
            } else if constexpr (std::is_same_v<T, List>) {
              for (auto& item : node.items) {
                ParseInlines(item.children, parser);
              }
            } else if constexpr (std::is_same_v<T, Table>) {
              for (auto& row : node.rows) {
                for (auto& cell : row.cells) {
                  cell.children =
                      parser.Parse(StableText(cell.raw_content, parser));
                }
              }
            }
          },
          block);
    }
  }
};

// =============================================================================
// HTML Renderer
// =============================================================================

// Safe mode: whether a link/image destination uses a scheme that can run
// script or read local files. Mirrors cmark's _scan_dangerous_url:
// `javascript:`, `vbscript:`, `file:` and `data:` (except the image types
// data:image/png, gif, jpeg and webp), compared case-insensitively.
inline bool IsDangerousUrl(std::string_view url) {
  if (detail::StartsWithInsensitive(url, "data:")) {
    return !(detail::StartsWithInsensitive(url, "data:image/png") ||
             detail::StartsWithInsensitive(url, "data:image/gif") ||
             detail::StartsWithInsensitive(url, "data:image/jpeg") ||
             detail::StartsWithInsensitive(url, "data:image/webp"));
  }
  return detail::StartsWithInsensitive(url, "javascript:") ||
         detail::StartsWithInsensitive(url, "vbscript:") ||
         detail::StartsWithInsensitive(url, "file:");
}

class HtmlRenderer {
 public:
  // GFM `tagfilter` (Disallowed Raw HTML) extension: when set, the leading
  // '<' of disallowed HTML tags in raw HTML blocks/inlines is escaped to
  // "&lt;".
  bool enable_tagfilter = false;

  // Safe mode (see Options::safe): omit raw HTML and blank out dangerous
  // link/image destinations.
  bool safe = false;

  std::pmr::string Render(const Document& doc) {
    doc_ = &doc;
    std::pmr::string result;
    // Pre-allocate from the inline text size plus markup per block, so the
    // output rarely has to grow (each growth step copies everything so far).
    size_t text_size = 0;
    for (const auto& s : doc.string_storage) text_size += s.size();
    size_t block_count = doc.children.size() + doc.block_nodes.size();
    size_t estimate = text_size + text_size / 8 + block_count * 16 + 256;
    size_t legacy_estimate = doc.children.size() * 100 + 256;
    result.reserve(estimate > legacy_estimate ? estimate : legacy_estimate);
    RenderBlocks(doc.children, result, false);
    return result;
  }

 private:
  const Document* doc_ = nullptr;
  void RenderBlocks(const std::pmr::vector<BlockNode>& blocks,
                    std::pmr::string& out, bool in_tight_list) {
    for (size_t i = 0; i < blocks.size(); ++i) {
      std::visit(
          [this, &out, in_tight_list](auto&& node) {
            using T = std::decay_t<decltype(node)>;
            if constexpr (std::is_same_v<T, Paragraph>) {
              RenderParagraph(node, out, in_tight_list);
            } else if constexpr (std::is_same_v<T, Heading>) {
              RenderHeading(node, out);
            } else if constexpr (std::is_same_v<T, ThematicBreak>) {
              out += "<hr />\n";
            } else if constexpr (std::is_same_v<T, CodeBlock>) {
              RenderCodeBlock(node, out);
            } else if constexpr (std::is_same_v<T, DetailsBlock>) {
              RenderDetailsBlock(node, out);
            } else if constexpr (std::is_same_v<T, HtmlBlock>) {
              RenderHtmlBlock(node, out);
            } else if constexpr (std::is_same_v<T, BlockQuote>) {
              RenderBlockQuote(node, out);
            } else if constexpr (std::is_same_v<T, List>) {
              RenderList(node, out);
            } else if constexpr (std::is_same_v<T, ListItem>) {
              // Handled by RenderList
            } else if constexpr (std::is_same_v<T, Table>) {
              RenderTable(node, out);
            }
          },
          blocks[i]);
    }
  }

  void RenderParagraph(const Paragraph& para, std::pmr::string& out,
                       bool in_tight_list) {
    if (in_tight_list) {
      RenderInlines(para.children, out);
      out += '\n';
    } else {
      out += "<p>";
      RenderInlines(para.children, out);
      out += "</p>\n";
    }
  }

  void RenderHeading(const Heading& heading, std::pmr::string& out) {
    // Precomputed heading tags for levels 1-6 (avoids std::to_string)
    static constexpr const char* kOpenTags[] = {"",     "<h1>", "<h2>", "<h3>",
                                                "<h4>", "<h5>", "<h6>"};
    static constexpr const char* kCloseTags[] = {
        "", "</h1>\n", "</h2>\n", "</h3>\n", "</h4>\n", "</h5>\n", "</h6>\n"};
    int level = heading.level;
    if (level >= 1 && level <= 6) {
      out += kOpenTags[level];
      RenderInlines(heading.children, out);
      out += kCloseTags[level];
    }
  }

  void RenderCodeBlock(const CodeBlock& block, std::pmr::string& out) {
    if (!block.info_string.empty()) {
      size_t end = block.info_string.find_first_of(" \t");
      std::string_view lang =
          (end == std::string::npos)
              ? std::string_view(block.info_string)
              : std::string_view(block.info_string).substr(0, end);
      if (!lang.empty()) {
        out += "<pre><code class=\"language-";
        detail::EscapeHtmlTo(lang, out);
        out += "\">";
        detail::EscapeHtmlTo(block.content, out);
        out += "</code></pre>\n";
        return;
      }
    }
    out += "<pre><code>";
    detail::EscapeHtmlTo(block.content, out);
    out += "</code></pre>\n";
  }

  void RenderHtmlBlock(const HtmlBlock& block, std::pmr::string& out) {
    if (safe) {
      out += "<!-- raw HTML omitted -->\n";
    } else if (enable_tagfilter) {
      detail::tagfilter::RenderFilteredHtmlBlock(block.content, out);
    } else {
      out += block.content;
    }
  }

  void RenderBlockQuote(const BlockQuote& bq, std::pmr::string& out) {
    out += "<blockquote>\n";
    RenderBlockIds(bq.children, out, false);
    out += "</blockquote>\n";
  }

  // Render blocks from a vector of BlockNodeIds
  void RenderBlockIds(const std::pmr::vector<BlockNodeId>& block_ids,
                      std::pmr::string& out, bool in_tight_list) {
    for (BlockNodeId id : block_ids) {
      const auto& block = doc_->block_nodes[id];
      std::visit(
          [this, &out, in_tight_list](auto&& node) {
            using T = std::decay_t<decltype(node)>;
            if constexpr (std::is_same_v<T, Paragraph>) {
              RenderParagraph(node, out, in_tight_list);
            } else if constexpr (std::is_same_v<T, Heading>) {
              RenderHeading(node, out);
            } else if constexpr (std::is_same_v<T, ThematicBreak>) {
              out += "<hr />\n";
            } else if constexpr (std::is_same_v<T, CodeBlock>) {
              RenderCodeBlock(node, out);
            } else if constexpr (std::is_same_v<T, DetailsBlock>) {
              RenderDetailsBlock(node, out);
            } else if constexpr (std::is_same_v<T, HtmlBlock>) {
              RenderHtmlBlock(node, out);
            } else if constexpr (std::is_same_v<T, BlockQuote>) {
              RenderBlockQuote(node, out);
            } else if constexpr (std::is_same_v<T, List>) {
              RenderList(node, out);
            } else if constexpr (std::is_same_v<T, ListItem>) {
              // Handled by RenderList
            } else if constexpr (std::is_same_v<T, Table>) {
              RenderTable(node, out);
            }
          },
          block);
    }
  }

  void RenderDetailsBlock(const DetailsBlock& block, std::pmr::string& out) {
    out += "<details";
    for (const auto& [name, value] : block.attributes) {
      // The attributes come from raw HTML: safe mode keeps only `open`.
      if (safe &&
          !(name.size() == 4 && detail::StartsWithInsensitive(name, "open"))) {
        continue;
      }
      out += ' ';
      detail::EscapeHtmlTo(name, out);
      if (!value.empty()) {
        out += "=\"";
        for (char c : value) {
          // Keep the attribute parseable on the next round trip.
          if (c == '"') out += "&quot;";
          else if (c == '<') out += "&lt;";
          else if (c == '>') out += "&gt;";
          else out += c;
        }
        out += '"';
      }
    }
    out += "><summary>";
    RenderBlockIds(block.summary, out, /*in_tight_list=*/false);
    out += "</summary>";
    RenderBlockIds(block.children, out, /*in_tight_list=*/false);
    out += "</details>\n";
  }

  void RenderList(const List& list, std::pmr::string& out) {
    if (list.is_ordered) {
      if (list.start == 1) {
        out += "<ol>\n";
      } else {
        out += "<ol start=\"";
        char buf[12];
        auto [ptr, ec] = std::to_chars(buf, buf + 12, list.start);
        out.append(buf, ptr - buf);
        out += "\">\n";
      }
    } else {
      out += "<ul>\n";
    }

    for (const auto& item : list.items) {
      out += "<li>";
      if (item.is_tasklist) {
        // GFM `tasklist` extension: emit the checkbox right after the <li>,
        // matching cmark-gfm's attribute order and trailing space.
        out += item.tasklist_checked
                   ? "<input type=\"checkbox\" checked=\"\" disabled=\"\" /> "
                   : "<input type=\"checkbox\" disabled=\"\" /> ";
      }

      if (list.is_tight && item.children.size() == 1) {
        // Look up the first block by ID
        const auto& first_block = doc_->block_nodes[item.children[0]];
        if (std::holds_alternative<Paragraph>(first_block)) {
          // Tight list with single paragraph - render without <p> tags
          const auto& para = std::get<Paragraph>(first_block);
          RenderInlines(para.children, out);
        } else {
          out += '\n';
          RenderBlockIds(item.children, out, true);
        }
      } else if (list.is_tight) {
        // Tight list with multiple blocks
        out += '\n';
        RenderBlockIds(item.children, out, true);
      } else {
        // Loose list
        out += '\n';
        RenderBlockIds(item.children, out, false);
      }

      out += "</li>\n";
    }

    if (list.is_ordered) {
      out += "</ol>\n";
    } else {
      out += "</ul>\n";
    }
  }

  void RenderTable(const Table& table, std::pmr::string& out) {
    out += "<table>\n";

    // Header row wrapped in <thead>. (The parser always produces one; guard
    // against hand-built ASTs.)
    if (table.rows.empty()) {
      out += "</table>\n";
      return;
    }
    out += "<thead>\n";
    const TableRow& header = table.rows.front();
    out += "<tr>\n";
    for (size_t i = 0; i < header.cells.size(); ++i) {
      RenderTableCell(header.cells[i], i, table.alignments, /*is_header=*/true,
                      out);
    }
    out += "</tr>\n</thead>\n";

    // Body rows wrapped in <tbody> (only emitted when there are body rows).
    if (table.rows.size() > 1) {
      out += "<tbody>\n";
      for (size_t r = 1; r < table.rows.size(); ++r) {
        const TableRow& row = table.rows[r];
        out += "<tr>\n";
        for (size_t i = 0; i < row.cells.size(); ++i) {
          RenderTableCell(row.cells[i], i, table.alignments,
                          /*is_header=*/false, out);
        }
        out += "</tr>\n";
      }
      out += "</tbody>\n";
    }

    out += "</table>\n";
  }

  void RenderTableCell(const TableCell& cell, size_t col,
                       const std::pmr::vector<TableAlign>& alignments,
                       bool is_header, std::pmr::string& out) {
    out += is_header ? "<th" : "<td";
    if (col < alignments.size()) {
      switch (alignments[col]) {
        case TableAlign::kLeft:
          out += " align=\"left\"";
          break;
        case TableAlign::kCenter:
          out += " align=\"center\"";
          break;
        case TableAlign::kRight:
          out += " align=\"right\"";
          break;
        default:
          break;
      }
    }
    out += '>';
    RenderInlines(cell.children, out);
    out += is_header ? "</th>\n" : "</td>\n";
  }

  void RenderInlines(const std::pmr::vector<InlineNodeId>& node_ids,
                     std::pmr::string& out) {
    for (InlineNodeId id : node_ids) {
      const auto& node = doc_->inline_nodes[id];
      std::visit(
          [this, &out](auto&& n) {
            using T = std::decay_t<decltype(n)>;
            if constexpr (std::is_same_v<T, Text>) {
              detail::EscapeHtmlTo(n.content, out);
            } else if constexpr (std::is_same_v<T, SoftBreak>) {
              out += '\n';
            } else if constexpr (std::is_same_v<T, HardBreak>) {
              out += "<br />\n";
            } else if constexpr (std::is_same_v<T, Code>) {
              out += "<code>";
              detail::EscapeHtmlTo(n.content, out);
              out += "</code>";
            } else if constexpr (std::is_same_v<T, Emphasis>) {
              out += "<em>";
              RenderInlines(n.children, out);
              out += "</em>";
            } else if constexpr (std::is_same_v<T, Strong>) {
              out += "<strong>";
              RenderInlines(n.children, out);
              out += "</strong>";
            } else if constexpr (std::is_same_v<T, Strikethrough>) {
              out += "<del>";
              RenderInlines(n.children, out);
              out += "</del>";
            } else if constexpr (std::is_same_v<T, Math>) {
              // KaTeX-style wrappers (the convention used by cmark's math
              // extension and most renderers).
              if (n.display) {
                out += "<span class=\"math display\">\\[";
                detail::EscapeHtmlTo(n.content, out);
                out += "\\]</span>";
              } else {
                out += "<span class=\"math inline\">\\(";
                detail::EscapeHtmlTo(n.content, out);
                out += "\\)</span>";
              }
            } else if constexpr (std::is_same_v<T, Link>) {
              out += "<a href=\"";
              if (!(safe && IsDangerousUrl(n.destination))) {
                detail::EscapeHtmlTo(n.destination, out);
              }
              out += '\"';
              if (!n.title.empty()) {
                out += " title=\"";
                detail::EscapeHtmlTo(n.title, out);
                out += '\"';
              }
              out += ">";
              RenderInlines(n.children, out);
              out += "</a>";
            } else if constexpr (std::is_same_v<T, Image>) {
              out += "<img src=\"";
              if (!(safe && IsDangerousUrl(n.destination))) {
                detail::EscapeHtmlTo(n.destination, out);
              }
              out += "\" alt=\"";
              detail::EscapeHtmlTo(n.alt_text, out);
              out += '"';
              if (!n.title.empty()) {
                out += " title=\"";
                detail::EscapeHtmlTo(n.title, out);
                out += '\"';
              }
              out += " />";
            } else if constexpr (std::is_same_v<T, HtmlInline>) {
              if (safe) {
                out += "<!-- raw HTML omitted -->";
              } else if (enable_tagfilter &&
                         detail::tagfilter::IsDisallowedTag(n.content)) {
                out.append("&lt;");
                out.append(n.content.substr(1));
              } else {
                out += n.content;
              }
            }
          },
          node);
    }
  }
};

// =============================================================================
// Public API
// =============================================================================

// Parser options. Plain CommonMark by default; individual GFM extensions can
// be turned on: `enable_tables` (the `table` extension), `enable_autolink`
// (the `autolink` extension, which recognises www/url/email autolinks in plain
// text in addition to the CommonMark `<...>` autolinks),
// `enable_strikethrough` (the `strikethrough` extension, which wraps `~`/`~~`
// delimited text in a <del> node), `enable_tasklist` (the `tasklist`
// extension, which renders list items beginning with a `[ ]`/`[x]`/`[X]` marker
// as checkboxes), and `enable_tagfilter` (the `tagfilter` / "Disallowed Raw
// HTML" extension, which escapes the leading '<' of a fixed set of HTML tags
// such as <title>, <style>, <script> and <xmp> in raw HTML output; it does not
// filter URL schemes such as `javascript:` - that is a consumer/sanitiser
// concern, matching cmark-gfm/GFM). `enable_latex_math` (the `latexmath`
// extension, mirroring md4c's MD_FLAG_LATEXMATHSPANS) recognises `$...$`
// (inline) and `$$...$$` (display) LaTeX math spans with verbatim content.
//
// `safe` selects cmark's safe rendering (its default unless `--unsafe` is
// given): raw HTML blocks and inline HTML are replaced by
// `<!-- raw HTML omitted -->`, link and image destinations with a
// `javascript:`, `vbscript:`, `file:` or non-image `data:` scheme are
// rendered empty, and <details> sections keep only their `open` attribute.
// It is off by default, as CommonMark itself passes raw HTML through; turn it
// on when rendering untrusted input (a full HTML sanitiser is still advisable
// for anything security-critical).
struct Options {
  bool enable_tables = false;
  bool enable_autolink = false;
  bool enable_strikethrough = false;
  bool enable_tasklist = false;
  bool enable_tagfilter = false;
  bool enable_latex_math = false;
  bool safe = false;
};

// Parse Markdown input and return an AST
inline Document Parse(std::string_view input) {
  BlockParser parser;
  return parser.Parse(input);
}

// Parse Markdown input with options and return an AST
inline Document Parse(std::string_view input, const Options& options) {
  BlockParser parser;
  parser.enable_tables = options.enable_tables;
  parser.enable_autolink = options.enable_autolink;
  parser.enable_strikethrough = options.enable_strikethrough;
  parser.enable_tasklist = options.enable_tasklist;
  parser.enable_latex_math = options.enable_latex_math;
  return parser.Parse(input);
}

// Render a document AST to HTML
inline std::pmr::string RenderHtml(const Document& doc) {
  HtmlRenderer renderer;
  return renderer.Render(doc);
}

// Render a document AST to HTML, applying the render-time options from
// `options` (the `tagfilter` / Disallowed Raw HTML extension and safe mode).
inline std::pmr::string RenderHtml(const Document& doc,
                                   const Options& options) {
  HtmlRenderer renderer;
  renderer.enable_tagfilter = options.enable_tagfilter;
  renderer.safe = options.safe;
  return renderer.Render(doc);
}

// Convenience function: parse Markdown and render to HTML
inline std::pmr::string MarkdownToHtml(std::string_view input) {
  return RenderHtml(Parse(input));
}

// Convenience function: parse Markdown with options and render to HTML
inline std::pmr::string MarkdownToHtml(std::string_view input,
                                       const Options& options) {
  return RenderHtml(Parse(input, options), options);
}

// =============================================================================
// Streaming API - progressive rendering of incrementally-arriving Markdown
// =============================================================================
//
// `StreamingMarkdownParser` accepts Markdown in arbitrary chunks (e.g. as it
// arrives over a network) and emits finished blocks as HTML through a callback
// as soon as they can no longer change. This is useful for progressive
// rendering where the full document is not available up front.
//
// Semantics:
// - `Feed(chunk)` appends to the internal buffer and emits every block that is
//   now guaranteed to be final. A block is final once its terminating line has
//   been seen (or it is an inherently atomic block such as a heading).
// - The trailing block that may still grow (an open paragraph, block quote,
//   list, code block, ...) is held back until `Feed` supplies its terminator or
//   `Flush()` is called. Containers that can be re-opened after a blank line
//   (block quotes, lists) are held back until definitively terminated, so a
//   further `> ` or item line rejoins the same block rather than starting a new
//   one.
// - `Feed` only re-parses when the chunk contains a line terminator; a chunk
//   without one cannot finalize any block, so it is buffered at (near) zero
//   cost. This is what keeps byte/char-by-byte streaming (e.g. LLM output)
//   cheap. Re-parses are paced by the cost of the previous one (see
//   kStreamReparseDivisor), so the total work stays linear in the input.
// - The held-back buffer is capped (default 4 MiB, see `setPendingLimit`). A
//   chunk that would push the buffer past the limit throws
//   `std::length_error` and leaves the buffer unchanged.
// - `Flush()` emits any remaining buffered content, treating the end of input
//   as a block terminator.
// - `Reset()` discards all buffered state (the output callback is retained).
// - Each block's HTML is emitted exactly once and never revised.
//
// Link references: definitions are remembered across chunks, so a reference
// defined before its use (the common case) resolves correctly even when they
// arrive in different chunks. A reference used before it is defined cannot be
// resolved retroactively (the definition has not arrived yet); it is rendered
// literally, matching the limitation inherent to any one-pass stream.
//
// Example:
//   StreamingMarkdownParser parser;
//   parser.setOutputCallback([](std::string_view html) {
//     write_to_network(html);
//   });
//   parser.Feed(chunk1);
//   parser.Feed(chunk2);
//   parser.Flush();

// Byte offset in `s` where 0-based line `line_idx` begins, counting line
// starts exactly the way detail::LineBuffer does (a line is terminated by
// '\n', or by '\r' optionally followed by '\n'). This scans the raw bytes, so
// it stays correct even when the input contains NULs (where LineBuffer would
// copy and its offsets would no longer index `s`). Linear in `s`.
inline size_t ByteOffsetOfLine(const std::string& s, size_t line_idx) {
  size_t line = 0;
  size_t line_start = 0;
  for (size_t i = 0; i < s.size(); ++i) {
    char c = s[i];
    if (c == '\n') {
      if (line == line_idx) return line_start;
      ++line;
      line_start = i + 1;
    } else if (c == '\r') {
      if (line == line_idx) return line_start;
      ++line;
      if (i + 1 < s.size() && s[i + 1] == '\n') ++i;
      line_start = i + 1;
    }
  }
  if (line == line_idx) return line_start;
  return s.size();
}

// Length of the prefix of `s` made of whole lines, i.e. up to and including
// its last line terminator ('\n' or '\r'). A '\r' that ends `s` does not
// count: the '\n' of a "\r\n" pair may still arrive in the next chunk.
inline size_t SettledLength(const std::string& s) {
  for (size_t i = s.size(); i >= 1; --i) {
    char c = s[i - 1];
    if (c == '\n' || (c == '\r' && i < s.size())) return i;
  }
  return 0;
}

// Streaming re-parse policy shared by the streaming parsers. Every re-parse
// covers the whole held-back tail, so re-parsing on each line of a long open
// block (a big list, block quote, paragraph or unclosed fence) would cost
// O(n^2) overall. Instead, the next re-parse waits until new input amounting
// to 1/kStreamReparseDivisor of the cost of re-parsing the held-back tail has
// settled. That cost is the tail's size scaled by the amplification of the
// last parse (`work` bytes of parsing, i.e. the settled input plus nested
// container content, for `settled` bytes of input). The total parsing work
// thus stays linear in the input, whatever the cost of an individual parse.
// Small blocks are still re-checked about every line; output is unaffected,
// a large block may just be emitted a little later than the line that
// completed it (at the latest on Flush()).
inline constexpr size_t kStreamReparseDivisor = 4;

// Settled length the buffer must reach before the next re-parse, given the
// `held` tail left after a parse of `settled` bytes that did `work` bytes of
// parsing.
inline size_t NextStreamReparseAt(size_t held, size_t settled, size_t work) {
  if (held == 0 || settled == 0) return 0;
  size_t amplification = work / settled;  // >= 1
  return held + held / kStreamReparseDivisor * amplification;
}

using HtmlStreamCallback = std::function<void(std::string_view html)>;

class StreamingMarkdownParser {
 public:
  StreamingMarkdownParser() = default;
  explicit StreamingMarkdownParser(const Options& options) { SetOptions(options); }

  void SetOptions(const Options& options) {
    options_ = options;
    parser_.enable_tables = options.enable_tables;
    parser_.enable_autolink = options.enable_autolink;
    parser_.enable_strikethrough = options.enable_strikethrough;
    parser_.enable_tasklist = options.enable_tasklist;
    parser_.enable_latex_math = options.enable_latex_math;
  }

  const Options& options() const { return options_; }

  void setOutputCallback(HtmlStreamCallback callback) {
    on_html_ = std::move(callback);
  }

  // Set the maximum size of the held-back buffer (default 4 MiB). A `Feed`
  // that would push the buffer past this limit throws `std::length_error`.
  void setPendingLimit(size_t limit) { pending_limit_ = limit; }

  // Append a chunk of Markdown and emit any newly-finalized blocks.
  //
  // Nothing can finalize on a chunk without a newline, so that case is buffered
  // without re-parsing (the fast path that makes char/byte-by-byte streaming
  // cheap). The held-back buffer is capped by `pending_limit_`; a chunk that
  // would exceed it throws `std::length_error` and leaves the buffer unchanged.
  void Feed(std::string_view chunk) {
    if (chunk.empty()) return;
    // (Written so it cannot wrap when the limit was lowered below the
    // current buffer size.)
    if (chunk.size() > pending_limit_ ||
        pending_.size() > pending_limit_ - chunk.size()) {
      throw std::length_error("markus: streaming pending buffer limit exceeded");
    }
    pending_.append(chunk.data(), chunk.size());
    if (chunk.find_first_of("\r\n") == std::string_view::npos) return;
    EmitCompletedBlocks();
  }

  // Emit any remaining buffered content (end-of-input).
  void Flush() {
    reparse_at_ = 0;
    if (pending_.empty()) return;
    if (pending_.find('[') != std::string::npos) {
      parser_.ExtractLinkRefs(pending_, running_link_refs_);
    }
    const std::string html = RenderSeeded(pending_);
    pending_.clear();
    Emit(html);
  }

  // Discard all buffered content and link references.
  void Reset() {
    pending_.clear();
    running_link_refs_.clear();
    reparse_at_ = 0;
  }

  bool empty() const { return pending_.empty(); }

 private:
  // Render `input` (seeded with the running link references, which are moved
  // into the parse and back rather than copied) to a std::string.
  std::string RenderSeeded(std::string_view input) {
    Document doc = parser_.Parse(input, std::move(running_link_refs_));
    running_link_refs_ = std::move(doc.link_references);
    std::pmr::string html = RenderHtml(doc, options_);
    return std::string(html.data(), html.size());
  }

  // Pass `html` to the output callback. A copy of the callback is invoked so
  // the callback may replace itself (setOutputCallback) while it runs.
  void Emit(std::string_view html) {
    if (html.empty() || !on_html_) return;
    HtmlStreamCallback callback = on_html_;
    callback(html);
  }

  // Emit every block in `pending_` that is guaranteed final, leaving only the
  // trailing (possibly incomplete) top-level block buffered.
  //
  // How it works: the settled portion is `pending_` up to and including the
  // last '\n'. Rendering it yields a `last_top_block` with a termination flag
  // (see BlockParser::RecordLastTopBlock). If that block is terminated, the
  // whole settled portion is final and is emitted. If it is still open (an open
  // paragraph / block quote / list / code block / HTML block that can absorb a
  // further line), only the leading complete blocks are emitted and the open
  // block is held back from its first line. Holding the block back from its
  // first line (rather than guessing via a sentinel probe) is what lets
  // re-openable containers (lists, block quotes) be rejoined across the blank
  // lines that separate their items.
  void EmitCompletedBlocks() {
    if (pending_.empty()) return;

    // The final line of the buffer has no line terminator and may still grow,
    // so only whole lines can be considered. Everything after the last line
    // terminator is always held back.
    size_t line_end = SettledLength(pending_);
    if (line_end == 0) return;  // only a partial line; wait for more data
    if (line_end < reparse_at_) return;  // back-off, see NextStreamReparseAt

    const std::string settled = pending_.substr(0, line_end);

    // Remember any link reference definitions in the settled (whole-line)
    // portion. Extracting only from `settled` — not the held-back tail, whose
    // final line is still arriving — means an incomplete definition cannot lock
    // in a truncated destination via first-definition-wins. Extracting from the
    // whole settled portion (including the trailing open block) also makes
    // references defined inside it available to the prefix render via the seed.
    if (settled.find('[') != std::string::npos) {
      parser_.ExtractLinkRefs(settled, running_link_refs_);
    }

    // Full render of the settled portion; this also records the trailing
    // block's termination state on parser_ (which the prefix render below will
    // clobber, so it is read before that).
    const std::string full_html = RenderSeeded(settled);

    // `cut` is the byte offset in `pending_` up to which we emit now; anything
    // from `cut` onward is held back for later.
    size_t cut = line_end;
    if (parser_.has_last_top_block && !parser_.last_top_block_terminated) {
      cut = ByteOffsetOfLine(settled, parser_.last_top_block_start_line);
    }

    reparse_at_ = NextStreamReparseAt(
        line_end - cut, line_end, line_end + parser_.nested_content_bytes());
    if (cut == 0) return;  // whole buffer is a single incomplete block
    const std::string emit_html =
        (cut == line_end) ? full_html : RenderSeeded(pending_.substr(0, cut));
    pending_ = pending_.substr(cut);

    Emit(emit_html);
  }

  Options options_;
  BlockParser parser_;
  HtmlStreamCallback on_html_;
  std::string pending_;
  LinkRefMap running_link_refs_;
  size_t pending_limit_ = 4 * 1024 * 1024;
  // Settled length the buffer must reach before the next re-parse.
  size_t reparse_at_ = 0;
};

// =============================================================================
// AST streaming API - progressive rendering to custom targets (e.g. a GUI
// document model) without going through HTML.
// =============================================================================
//
// `StreamingBlockParser` accepts Markdown in arbitrary chunks and delivers
// finished *top-level blocks as an AST* through a callback as soon as they can
// no longer change. This is the counterpart of `StreamingMarkdownParser` for
// consumers that render from the AST instead of HTML.
//
// The callback receives the `Document` the blocks were parsed into together
// with the half-open range `[first, last)` of top-level blocks to render
// (`Document::children`). The Document is only guaranteed to be valid while
// the callback runs; everything the consumer needs (text, node pools, decoded
// strings) is read-only state of that Document. Its `link_references` are
// empty: references are already resolved into the inline nodes, and the
// parser keeps the running definitions itself.
//
// The finalization semantics match `StreamingMarkdownParser`: a block is
// emitted once its terminating line has been seen (or it is atomic); the
// trailing block that may still grow is held back until its terminator
// arrives or `Flush()` is called. Each top-level block is delivered exactly
// once, in order.

using BlockStreamCallback =
    std::function<void(const Document&, size_t first, size_t last)>;

class StreamingBlockParser {
 public:
  StreamingBlockParser() = default;
  explicit StreamingBlockParser(const Options& options) { SetOptions(options); }

  void SetOptions(const Options& options) {
    options_ = options;
    parser_.enable_tables = options.enable_tables;
    parser_.enable_autolink = options.enable_autolink;
    parser_.enable_strikethrough = options.enable_strikethrough;
    parser_.enable_tasklist = options.enable_tasklist;
    parser_.enable_latex_math = options.enable_latex_math;
  }

  const Options& options() const { return options_; }

  void setBlockCallback(BlockStreamCallback callback) {
    on_blocks_ = std::move(callback);
  }

  // Set the maximum size of the held-back buffer (default 4 MiB). A `Feed`
  // that would push the buffer past this limit throws `std::length_error`.
  void setPendingLimit(size_t limit) { pending_limit_ = limit; }

  // Append a chunk of Markdown and deliver any newly-finalized blocks.
  //
  // Nothing can finalize on a chunk without a newline, so that case is
  // buffered without re-parsing (the fast path that makes char/byte-by-byte
  // streaming cheap).
  void Feed(std::string_view chunk) {
    if (chunk.empty()) return;
    // (Written so it cannot wrap when the limit was lowered below the
    // current buffer size.)
    if (chunk.size() > pending_limit_ ||
        pending_.size() > pending_limit_ - chunk.size()) {
      throw std::length_error("markus: streaming pending buffer limit exceeded");
    }
    pending_.append(chunk.data(), chunk.size());
    if (chunk.find_first_of("\r\n") == std::string_view::npos) return;
    EmitCompletedBlocks();
  }

  // Deliver any remaining buffered content (end-of-input).
  void Flush() {
    reparse_at_ = 0;
    if (pending_.empty()) return;
    if (pending_.find('[') != std::string::npos) {
      parser_.ExtractLinkRefs(pending_, running_link_refs_);
    }
    Document doc = parser_.Parse(pending_, std::move(running_link_refs_));
    running_link_refs_ = std::move(doc.link_references);
    pending_.clear();
    Deliver(doc, 0, doc.children.size());
  }

  // Discard all buffered content and link references.
  void Reset() {
    pending_.clear();
    running_link_refs_.clear();
    reparse_at_ = 0;
  }

  bool empty() const { return pending_.empty(); }

  // The currently held-back tail: the trailing block that may still grow (or a
  // partial final line). Consumers that want to preview the in-progress block
  // can parse it with `Parse` and re-render it after every `Feed`; the tail is
  // not part of the finalised-block stream.
  const std::string& pending() const { return pending_; }

 private:
  // Deliver every block in `pending_` that is guaranteed final, leaving only
  // the trailing (possibly incomplete) top-level block buffered.
  //
  // The settled portion is `pending_` up to and including the last '\n'.
  // Parsing it yields a Document whose last top-level block carries the
  // termination flag recorded by BlockParser (see RecordLastTopBlock). If
  // that block is still open, it is held back: it is the last element of
  // `Document::children` and starts at `last_top_block_start_line`.
  // Pass blocks [first, last) of `doc` to the block callback. A copy of the
  // callback is invoked so it may replace itself while it runs.
  void Deliver(const Document& doc, size_t first, size_t last) {
    if (first == last || !on_blocks_) return;
    BlockStreamCallback callback = on_blocks_;
    callback(doc, first, last);
  }

  void EmitCompletedBlocks() {
    if (pending_.empty()) return;

    // The final line of the buffer has no line terminator and may still grow,
    // so only whole lines can be considered. Everything after the last line
    // terminator is always held back.
    size_t line_end = SettledLength(pending_);
    if (line_end == 0) return;  // only a partial line; wait for more data
    if (line_end < reparse_at_) return;  // back-off, see NextStreamReparseAt

    const std::string settled = pending_.substr(0, line_end);

    // Remember any link reference definitions in the settled (whole-line)
    // portion so references defined in an earlier chunk resolve in later
    // ones (first definition wins).
    if (settled.find('[') != std::string::npos) {
      parser_.ExtractLinkRefs(settled, running_link_refs_);
    }

    // The running link references are moved into the parse (not copied) and
    // back out before any callback runs, so a re-entrant Feed() sees them.
    Document doc = parser_.Parse(settled, std::move(running_link_refs_));
    running_link_refs_ = std::move(doc.link_references);

    size_t first = 0;
    size_t last = doc.children.size();
    size_t cut = line_end;
    if (parser_.has_last_top_block && !parser_.last_top_block_terminated &&
        last > first) {
      // The trailing top-level block may still grow: hold it back.
      --last;
      cut = ByteOffsetOfLine(settled, parser_.last_top_block_start_line);
    }
    reparse_at_ = NextStreamReparseAt(
        line_end - cut, line_end, line_end + parser_.nested_content_bytes());
    pending_ = pending_.substr(cut);

    Deliver(doc, first, last);
  }

  Options options_;
  BlockParser parser_;
  BlockStreamCallback on_blocks_;
  std::string pending_;
  LinkRefMap running_link_refs_;
  size_t pending_limit_ = 4 * 1024 * 1024;
  // Settled length the buffer must reach before the next re-parse.
  size_t reparse_at_ = 0;
};

// Convenience: stream the whole input through the callback (feed + flush).
inline void StreamMarkdownToHtml(std::string_view input,
                                 HtmlStreamCallback callback,
                                 const Options& options = Options{}) {
  StreamingMarkdownParser parser(options);
  parser.setOutputCallback(std::move(callback));
  parser.Feed(input);
  parser.Flush();
}

// Debug: print AST structure
// Debug suffix appended to a list item line when it is a GFM task list item.
inline const char* TasklistDebugSuffix(const ListItem& item) {
  if (!item.is_tasklist) {
    return "";
  }
  return item.tasklist_checked ? " (tasklist checked)" : " (tasklist)";
}

inline std::pmr::string DebugAst(const Document& doc, int indent = 0) {
  std::pmr::string result;
  std::pmr::string prefix(indent * 2, ' ');

  result += prefix + "Document\n";

  std::function<void(const std::pmr::vector<BlockNode>&, int)> print_blocks;
  std::function<void(const std::pmr::vector<BlockNodeId>&, int)>
      print_block_ids;
  std::function<void(const std::pmr::vector<InlineNodeId>&, int)> print_inlines;

  print_inlines = [&](const std::pmr::vector<InlineNodeId>& node_ids, int ind) {
    std::pmr::string p(ind * 2, ' ');
    for (InlineNodeId id : node_ids) {
      const auto& node = doc.inline_nodes[id];
      std::visit(
          [&](auto&& n) {
            using T = std::decay_t<decltype(n)>;
            if constexpr (std::is_same_v<T, Text>) {
              result += p;
              result += "Text: \"";
              result += n.content;
              result += "\"\n";
            } else if constexpr (std::is_same_v<T, SoftBreak>) {
              result += p + "SoftBreak\n";
            } else if constexpr (std::is_same_v<T, HardBreak>) {
              result += p + "HardBreak\n";
            } else if constexpr (std::is_same_v<T, Code>) {
              result += p + "Code: \"" + n.content + "\"\n";
            } else if constexpr (std::is_same_v<T, Emphasis>) {
              result += p + "Emphasis\n";
              print_inlines(n.children, ind + 1);
            } else if constexpr (std::is_same_v<T, Strong>) {
              result += p + "Strong\n";
              print_inlines(n.children, ind + 1);
            } else if constexpr (std::is_same_v<T, Strikethrough>) {
              result += p + "Strikethrough\n";
              print_inlines(n.children, ind + 1);
            } else if constexpr (std::is_same_v<T, Math>) {
              result += p;
              result += n.display ? "MathDisplay: \"" : "Math: \"";
              result += n.content;
              result += "\"\n";
            } else if constexpr (std::is_same_v<T, Link>) {
              result += p + "Link: " + n.destination + "\n";
              print_inlines(n.children, ind + 1);
            } else if constexpr (std::is_same_v<T, Image>) {
              result += p + "Image: " + n.destination + "\n";
            } else if constexpr (std::is_same_v<T, HtmlInline>) {
              result += p;
              result += "HtmlInline: ";
              result += n.content;
              result += "\n";
            }
          },
          node);
    }
  };

  print_block_ids = [&](const std::pmr::vector<BlockNodeId>& block_ids,
                        int ind) {
    std::pmr::string p(ind * 2, ' ');
    for (BlockNodeId id : block_ids) {
      const auto& block = doc.block_nodes[id];
      std::visit(
          [&](auto&& n) {
            using T = std::decay_t<decltype(n)>;
            if constexpr (std::is_same_v<T, Paragraph>) {
              result += p + "Paragraph\n";
              print_inlines(n.children, ind + 1);
            } else if constexpr (std::is_same_v<T, Heading>) {
              result += std::format("{}Heading (level {})\n", p, n.level);
              print_inlines(n.children, ind + 1);
            } else if constexpr (std::is_same_v<T, ThematicBreak>) {
              result += p + "ThematicBreak\n";
            } else if constexpr (std::is_same_v<T, CodeBlock>) {
              result += std::format("{}CodeBlock{}", p,
                                    n.info_string.empty()
                                        ? ""
                                        : std::format(" ({})", n.info_string));
              result += "\n";
            } else if constexpr (std::is_same_v<T, DetailsBlock>) {
              result += std::format(
                  "{}DetailsBlock ({} summary block(s), {})\n", p,
                  n.summary.size(), n.closed ? "closed" : "open");
              print_block_ids(n.summary, ind + 1);
              print_block_ids(n.children, ind + 1);
            } else if constexpr (std::is_same_v<T, HtmlBlock>) {
              result += std::format("{}HtmlBlock (type {})\n", p, n.block_type);
            } else if constexpr (std::is_same_v<T, BlockQuote>) {
              result += p + "BlockQuote\n";
              print_block_ids(n.children, ind + 1);
            } else if constexpr (std::is_same_v<T, List>) {
              result += p + "List (" +
                        (n.is_ordered ? "ordered" : "unordered") +
                        (n.is_tight ? ", tight" : ", loose") + ")\n";
              for (const auto& item : n.items) {
                result += p + "  ListItem" + TasklistDebugSuffix(item) + "\n";
                print_block_ids(item.children, ind + 2);
              }
            } else if constexpr (std::is_same_v<T, ListItem>) {
              result += p + "ListItem" + TasklistDebugSuffix(n) + "\n";
              print_block_ids(n.children, ind + 1);
            } else if constexpr (std::is_same_v<T, Table>) {
              result +=
                  std::format("{}Table ({} cols)\n", p, n.alignments.size());
              for (const auto& row : n.rows) {
                result += p + (row.is_header ? "  Row (header)\n" : "  Row\n");
                for (const auto& cell : row.cells) {
                  result += p + "    Cell\n";
                  print_inlines(cell.children, ind + 3);
                }
              }
            }
          },
          block);
    }
  };

  print_blocks = [&](const std::pmr::vector<BlockNode>& blocks, int ind) {
    std::pmr::string p(ind * 2, ' ');
    for (const auto& block : blocks) {
      std::visit(
          [&](auto&& n) {
            using T = std::decay_t<decltype(n)>;
            if constexpr (std::is_same_v<T, Paragraph>) {
              result += p + "Paragraph\n";
              print_inlines(n.children, ind + 1);
            } else if constexpr (std::is_same_v<T, Heading>) {
              result += std::format("{}Heading (level {})\n", p, n.level);
              print_inlines(n.children, ind + 1);
            } else if constexpr (std::is_same_v<T, ThematicBreak>) {
              result += p + "ThematicBreak\n";
            } else if constexpr (std::is_same_v<T, CodeBlock>) {
              result += std::format("{}CodeBlock{}", p,
                                    n.info_string.empty()
                                        ? ""
                                        : std::format(" ({})", n.info_string));
              result += "\n";
            } else if constexpr (std::is_same_v<T, DetailsBlock>) {
              result += std::format(
                  "{}DetailsBlock ({} summary block(s), {})\n", p,
                  n.summary.size(), n.closed ? "closed" : "open");
              print_block_ids(n.summary, ind + 1);
              print_block_ids(n.children, ind + 1);
            } else if constexpr (std::is_same_v<T, HtmlBlock>) {
              result += std::format("{}HtmlBlock (type {})\n", p, n.block_type);
            } else if constexpr (std::is_same_v<T, BlockQuote>) {
              result += p + "BlockQuote\n";
              print_block_ids(n.children, ind + 1);
            } else if constexpr (std::is_same_v<T, List>) {
              result += p + "List (" +
                        (n.is_ordered ? "ordered" : "unordered") +
                        (n.is_tight ? ", tight" : ", loose") + ")\n";
              for (const auto& item : n.items) {
                result += p + "  ListItem" + TasklistDebugSuffix(item) + "\n";
                print_block_ids(item.children, ind + 2);
              }
            } else if constexpr (std::is_same_v<T, ListItem>) {
              result += p + "ListItem" + TasklistDebugSuffix(n) + "\n";
              print_block_ids(n.children, ind + 1);
            } else if constexpr (std::is_same_v<T, Table>) {
              result +=
                  std::format("{}Table ({} cols)\n", p, n.alignments.size());
              for (const auto& row : n.rows) {
                result += p + (row.is_header ? "  Row (header)\n" : "  Row\n");
                for (const auto& cell : row.cells) {
                  result += p + "    Cell\n";
                  print_inlines(cell.children, ind + 3);
                }
              }
            }
          },
          block);
    }
  };

  print_blocks(doc.children, indent + 1);
  return result;
}

}  // namespace markus

#endif  // MARKUS_H_
