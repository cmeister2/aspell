#include "settings.h"

#include "config.hpp"
#include "indiv_filter.hpp"
#include "iostream.hpp"
#include "sgml.hpp"

#include <typeinfo>

using namespace aspell_filters;

namespace {

struct Iterator;

struct Block {
  Block * next;
  Block() : next() {}
  enum KeepOpenState {NEVER, MAYBE, YES};
  virtual KeepOpenState proc_line(Iterator &) = 0;
  virtual void dump() const = 0;
  virtual bool leaf() const = 0;
  virtual ~Block() {}
};

struct DocRoot : Block {
  KeepOpenState proc_line(Iterator &) {return YES;}
  void dump() const {CERR.printf("DocRoot\n");}
  bool leaf() const {return false;}
};

struct MultilineInlineState;

class MarkdownFilter : public IndividualFilter {
public:
  MarkdownFilter() : root(), back(&root), prev_blank(true), inline_state() {
    name_ = "markdown-filter";
    order_num_ = 0.35;
    //html_filter = new_html_filter();
  }
  PosibErr<bool> setup(Config *);
  void reset();
  ~MarkdownFilter();

  void process(FilterChar * & start, FilterChar * & stop);

private:
  //SgmlFilter * html_filter;

  void dump() {
    CERR.printf(">>>blocks\n");
    for (Block * cur = &root; cur; cur = cur->next) {
      cur->dump();
    }
    CERR.printf("<<<blocks\n");
  }

  bool multiline_tags;
  StringMap raw_start_tags;
  StringMap block_start_tags;
  
  DocRoot root;
  Block * back;
  bool prev_blank;
  MultilineInlineState * inline_state;
 
  void kill(Block * blk) {
    Block * cur = &root;
    while (cur->next && cur->next != blk)
      cur = cur->next;
    back = cur;
    Block * next = cur->next;
    cur->next = NULL;
    cur = next;
    while (cur) {
      next = cur->next;
      delete cur;
      cur = cur->next;
    }
  }

  void add(Block * blk) {
    back->next = blk;
    back = blk;
  }

  Block * start_block(Iterator & itr);
};

//
// Iterator class
//

inline void blank(FilterChar & chr) {
  if (!asc_isspace(chr))
    chr = ' ';
}

struct Iterator {
  FilterChar * i;
  FilterChar * end;
  int line_pos;
  int indent;
  Iterator(FilterChar * start, FilterChar * stop)
    : i(start), end(stop), line_pos(), indent() {}
  void * pos() {return i;}
  unsigned int operator[](unsigned x) const {
    if (i + x >= end) return '\0';
    if (*i == '\r' || *i == '\n') return '\0';
    else return i[x];
  }
  unsigned int operator *() const {return operator[](0); }
  bool eol() const {return operator*() == '\0';}
  bool eos() const {return i >= end;}
  int width() const {
    if (i == end) return 0;
    if (*i == '\t') return 4  - (line_pos % 4);
    return 1;
  }
  bool eq(const char * str) {
    int i = 0;
    while (str[i] != '\0' && operator[](i) == str[i])
      ++i;
    return str[i] == '\0';
  }
  void inc() {
    indent = 0;
    if (i == end) return;
    line_pos += width();
    ++i;
  }
  void adv(int width = 1) {
    for (; width > 0; --width)
      inc();
    eat_space();
  }
  void blank_adv(int width = 1) {
    for (; !eol() && width > 0; --width) {
      blank(*i);
      inc();
    }
    eat_space();
  }
  void blank_rest() {
    while (!eol()) {
      blank(*i);
      inc();
    }
  }
  int eat_space();
  void next_line();
};

int Iterator::eat_space() {
  indent = 0;
  while (!eol()) {
    if (*i == ' ') {
      ++i;
      indent++;
      line_pos++;
    } else if (*i == '\t') {
      int w = width();
      ++i;
      indent += w;
      line_pos += w;
    } else {
      break;
    }
  }
  return indent;
}

void Iterator::next_line() {
  while (!eol())
    inc();
  if (!eos() && *i == '\n') {
    ++i;
    if (!eos() && *i == '\r') {
      ++i;
    }
  } else if (!eos() && *i == '\r') {
    ++i;
  }
  line_pos = 0;
}

//
// Markdown blocks and inlines
// 

struct BlockQuote : Block {
  static BlockQuote * start_block(Iterator & itr) {
    if (*itr == '>') {
      itr.blank_adv();
      return new BlockQuote();
    }
    return NULL;
  }
  KeepOpenState proc_line(Iterator & itr) {
    if (*itr == '>') {
      itr.blank_adv();
      return YES;
    } else if (itr.eol()) {
      return NEVER;
    }
    return MAYBE;
  }
  void dump() const {CERR.printf("BlockQuote\n");}
  bool leaf() const {return false;}
};

struct ListItem : Block {
  char marker; // '-' '+' or '*' for bullet lists; '.' or ')' for ordered lists
  int indent; // indention required in order to be considered part of
              // the same list item
  ListItem(char m, int i)
    : marker(m), indent(i) {}
  static ListItem * start_block(Iterator & itr) {
    char marker = '\0';
    int width = 0;
    if (*itr == '-' || *itr == '+' || *itr == '*') {
      marker = *itr;
      width = 1;
    } else if (asc_isdigit(*itr)) {
      width = 1;
      while (asc_isdigit(itr[width]))
        width += 1;
      if (itr[width] == '.' || itr[width] == ')') {
        width += 1;
        marker = *itr;
      }
    }
    if (marker != '\0') {
      itr.adv(width);
      if (itr.indent <= 4) {
        int indent = width + itr.indent;
        itr.indent = 0;
        return new ListItem(marker, indent);
      } else {
        int indent = 1 + itr.indent;
        itr.indent -= 1;
        return new ListItem(marker, indent);
      }
    }
    return NULL;
  }
  KeepOpenState proc_line(Iterator & itr) {
    if (!itr.eol() && itr.indent >= indent) {
      itr.indent -= indent;
      return YES;
    }
    return MAYBE;
  }
  void dump() const {CERR.printf("ListItem: '%c' %d\n", marker, indent);}
  bool leaf() const {return false;}
};

struct IndentedCodeBlock : Block {
  static IndentedCodeBlock * start_block(bool prev_blank, Iterator & itr) {
    if (prev_blank && !itr.eol() && itr.indent >= 4) {
      itr.indent -= 4;
      return new IndentedCodeBlock();
    }
    return NULL;
  }
  KeepOpenState proc_line(Iterator & itr) {
    if (itr.indent >= 4) {
      itr.blank_rest();
      return YES;
    } else if (itr.eol()) {
      return YES;
    }
    return NEVER;
  }
  void dump() const {CERR.printf("IndentedCodeBlock\n");}
  bool leaf() const {return true;}
};

struct FencedCodeBlock : Block {
  char delem;
  int  delem_len;
  FencedCodeBlock(char d, int l) : delem(d), delem_len(l) {}
  static FencedCodeBlock * start_block(Iterator & itr) {
    if (*itr == '`' || *itr == '~') {
      char delem = *itr;
      int i = 1;
      while (itr[i] == delem)
        ++i;
      if (i < 3) return NULL;
      itr.blank_adv(i);
      itr.blank_rest(); // blank info string
      return new FencedCodeBlock(delem, i);
    }
    return NULL;
  }
  KeepOpenState proc_line(Iterator & itr) {
    if (*itr == '`' || *itr == '~') {
      char delem = *itr;
      int i = 1;
      while (itr[i] == delem)
        ++i;
      itr.blank_adv(i);
      if (i >= delem_len && itr.eol()) {
        return NEVER;
      }
    }
    itr.blank_rest();
    return YES;
  }
  bool blank_rest() const {
    return true;
  }
  void dump() const {CERR.printf("FencedCodeBlock: `%c` %d\n", delem, delem_len);}
  bool leaf() const {return true;}
};

struct SingleLineBlock : Block {
  static SingleLineBlock * start_block(Iterator & itr) {
    unsigned int chr = *itr;
    switch (chr) {
    case '-': case '_': case '*': {
      Iterator i = itr;
      i.adv();
      while (*i == *itr)
        i.adv();
      if (i.eol()) {
        itr = i; 
        return new SingleLineBlock();
      }
      if (chr != '-') // fall though on '-' case
        break;
    }
    case '=': {
      Iterator i = itr;
      i.inc();
      while (*i == *itr)
        i.inc();
      i.eat_space();
      if (i.eol()) {
        itr = i;
        return new SingleLineBlock();
      }
      break;
    }
    case '#':
      return new SingleLineBlock();
      break;
    case '[': {
      Iterator i = itr;
      i.adv();
      if (*i == ']') break;
      while (*i != ']') {
        i.adv();
      }
      i.inc();
      if (*i != ':') break;
      return new SingleLineBlock();
    }
    }
    return NULL;
  }
  KeepOpenState proc_line(Iterator & itr) {return NEVER;}
  bool leaf() const {return true;}
  void dump() const {CERR.printf("SingleLineBlock\n");}
};

struct MultilineInline {
  virtual MultilineInline * close(Iterator & itr) = 0;
  virtual ~MultilineInline() {}
};

struct InlineCode : MultilineInline {
  int marker_len;
  MultilineInline * open(Iterator & itr) {
    if (*itr == '`') {
      int i = 1;
      while (itr[i] == '`')
        ++i;
      itr.blank_adv(i);
      marker_len = i;
      return close(itr);
    }
    return NULL;
  }
  MultilineInline * close(Iterator & itr) {
    while (!itr.eol()) {
      if (*itr == '`') {
        int i = 1;
        while (i < marker_len && itr[i] == '`')
          ++i;
        if (i == marker_len) {
          itr.blank_adv(i);
          return NULL;
        }
      }
      itr.blank_adv();
    }
    return this;
  }
};

struct HtmlComment : MultilineInline {
  MultilineInline * open(Iterator & itr) {
    if (itr.eq("<!--")) {
      itr.adv(4);
      return close(itr);
    }
    return NULL;
  }
  MultilineInline * close(Iterator & itr) {
    while (!itr.eol()) {
      if (itr.eq("-->")) {
        itr.adv(3);
        return NULL;
      }
      itr.inc();
    }
    return this;
  }
};

bool parse_tag_close(Iterator & itr) {
  if (*itr == '>') {
    itr.adv();
    return true;
  } else if (*itr == '/' && itr[1] == '>') {
    itr.adv(2);
    return true;
  }
  return false;
}

// note: does _not_ eat trialing whitespaceb
bool parse_tag_name(Iterator & itr, String & tag) {
  if (asc_isalpha(*itr)) {
    tag += static_cast<char>(*itr);
    itr.inc();
    while (asc_isalpha(*itr) || asc_isdigit(*itr) || *itr == '-') {
      tag += static_cast<char>(*itr);
      itr.inc();
    }
    return true;
  }
  return false;
}

enum ParseTagState {
  Invalid,Between,AfterName,AfterEq,InSingleQ,InDoubleQ,Valid
};

// note: does _not_ eat trialing whitespace
ParseTagState parse_attribute(Iterator & itr, ParseTagState state) {
  switch (state) {
    // note: this switch is being used as a computed goto to make
    //   restoring state straightforward without restructuring the code
  case Between:
    if (asc_isalpha(*itr) || *itr == '_' || *itr == ':') {
      itr.inc();
      while (asc_isalpha(*itr) || asc_isdigit(*itr)
             || *itr == '_' || *itr == ':' || *itr == '.' || *itr == '-')
        itr.inc();
    case AfterName:
      itr.eat_space();
      if (itr.eol()) return AfterName;
      if (*itr != '=') return Invalid;
      state = AfterEq;
      itr.inc();
    case AfterEq:
      itr.eat_space();
      if (itr.eol()) return AfterEq;
      if (*itr == '\'') {
        itr.inc();
      case InSingleQ:
        while (!itr.eol() && *itr != '\'')
          itr.inc();
        if (itr.eol()) return InSingleQ;
        if (*itr != '\'') return Invalid;
        itr.inc();
      } else if (*itr == '"') {
        itr.inc();
      case InDoubleQ:
        while (!itr.eol() && *itr != '"')
          itr.inc();
        if (itr.eol()) return InDoubleQ;
        if (*itr != '"') return Invalid;
        itr.inc();
      } else {
        void * pos = itr.pos();
        while (!itr.eol() && !asc_isspace(*itr)
               && *itr != '"' && *itr != '\'' && *itr != '='
               && *itr != '<' && *itr != '>' && *itr != '`')
          itr.inc();
        if (pos == itr.pos()) return Invalid;
      }
    }
    return Between;
  case Valid: case Invalid:
    // should not happen
    abort();
  }
}

struct HtmlTag : MultilineInline {
  HtmlTag(bool mlt) : multi_line_tags(mlt) {}
  void * start_pos; // used for caching
  String tag;
  bool closing;
  ParseTagState state;
  bool multi_line_tags;
  void reset() {
    start_pos = NULL;
    tag.clear();
    closing = false;
    state = Invalid;
  }
  MultilineInline * open(Iterator & itr) {
    if (itr.pos() == start_pos) {
      if (state != Invalid && state != Valid)
        return this;
      return NULL;
    }
    reset();
    start_pos = itr.pos();
    Iterator itr0 = itr;
    if (*itr == '<') {
      itr.inc();
      if (*itr == '/') {
        itr.inc();
        closing = true;
      }
      if (!parse_tag_name(itr, tag))
        return invalid(itr0, itr);
      state = Between;
      if (itr.eol()) {
        return incomplete(itr0, itr);
      } else if (parse_tag_close(itr)) {
        return valid();
      } else if (asc_isspace(*itr)) {
        return close(itr0, itr);
      } else {
        return invalid(itr0, itr);
      }
    }
    return NULL;
  }
  MultilineInline * close(const Iterator & itr0, Iterator & itr) {
    while (!itr.eol()) {
      if (state == Between) {
        bool leading_space = asc_isspace(*itr);
        if (leading_space)
          itr.eat_space();
        
        if (parse_tag_close(itr))
          return valid();
        
        if (itr.line_pos != 0 && !leading_space)
          return invalid(itr0, itr);
      }

      state = parse_attribute(itr, state);
      if (state == Invalid)
        return invalid(itr0, itr);
    }
    return incomplete(itr0, itr);
  }
  MultilineInline * close(Iterator & itr) {
    Iterator itr0 = itr;
    return close(itr0, itr);
  }

  MultilineInline * valid() {
    state = Valid;
    return NULL;
  }
  MultilineInline * invalid(const Iterator & itr0, Iterator & itr) {
    state = Invalid;
    itr = itr0;
    return NULL;
  }
  MultilineInline * incomplete(const Iterator & itr0, Iterator & itr) {
    if (multi_line_tags)
      return this;
    return invalid(itr0, itr);
  }
};

struct HtmlBlock : Block {
  KeepOpenState proc_line(Iterator & itr) {
    if (itr.eol()) return NEVER;
    while (!itr.eol()) itr.inc();
    return YES;
  }
  bool blank_rest() const {return false;}
  void dump() const {CERR.printf("HtmlBlock\n");}
  bool leaf() const {return false;}
};

Block * start_html_block(HtmlTag & tag, Iterator & itr) {
  tag.open(itr);
  if (tag.state == Valid) return new HtmlBlock();
  return NULL;
}

struct MultilineInlineState {
  MultilineInlineState(bool mlt) : ptr(), tag(mlt) {}
  MultilineInline * ptr;
  InlineCode inline_code;
  HtmlComment comment;
  HtmlTag tag;
  void reset() {
    tag.reset();
  }
};

//
// MarkdownFilter implementation
//

PosibErr<bool> MarkdownFilter::setup(Config * cfg) {
  bool multiline_tags = cfg->retrieve_bool("f-markdown-multiline-tags");
  if (inline_state)
    free(inline_state);
  inline_state = new MultilineInlineState(multiline_tags);
  raw_start_tags.clear();
  cfg->retrieve_list("f-markdown-raw-start-tags",  &raw_start_tags);
  block_start_tags.clear();
  cfg->retrieve_list("f-markdown-block-start-tags", &block_start_tags);

  //return html_filter->setup(cfg);
  return true;
}

void MarkdownFilter::reset() {
  // FIXME: Correctly implement me
  //html_filter->reset();
}


MarkdownFilter::~MarkdownFilter() {
  //delete html_filter;
}

void MarkdownFilter::process(FilterChar * & start, FilterChar * & stop) {
  inline_state->reset(); // to clear cached values
  Iterator itr(start,stop);
  bool blank_line = false;
  while (!itr.eos()) {
    if (inline_state->ptr) {
      CERR.printf("*** continuing multi-line inline %s\n", typeid(*inline_state->ptr).name());
      inline_state->ptr = inline_state->ptr->close(itr);
    } else {
      itr.eat_space();
      
      Block * blk = &root;
      Block::KeepOpenState keep_open;
      for (; blk; blk = blk->next) {
        keep_open = blk->proc_line(itr);
        if (keep_open != Block::YES)
          break;
      }

      blank_line = itr.eol();
      Block * nblk = blank_line || (keep_open == Block::YES && back->leaf())
        ? NULL
        : start_block(itr);
      
      if (nblk || keep_open == Block::NEVER || (prev_blank && !blank_line)) {
        CERR.printf("*** kill\n");
        kill(blk);
      } else {
        for (; blk; blk = blk->next) {
          keep_open = blk->proc_line(itr);
          if (keep_open == Block::NEVER) {
            CERR.printf("***** kill\n");
            kill(blk);
            break;
          }
        }
      }

      if (nblk) {
        CERR.printf("*** new block\n");
        add(nblk);
        prev_blank = true;
      }

      while (nblk && !nblk->leaf()) {
        nblk = start_block(itr);
        if (nblk) {
          CERR.printf("*** new block\n");
          add(nblk);
        }
      }

      dump();
    }
    // now process line, mainly blank inline code and handle html tags
      
    while (!itr.eol()) {
      inline_state->ptr = inline_state->inline_code.open(itr);
      if (inline_state->ptr) break;
      inline_state->ptr = inline_state->comment.open(itr);
      if (inline_state->ptr) break;
      inline_state->ptr = inline_state->tag.open(itr);
      if (inline_state->ptr) break;
      if (*itr == '<' || *itr == '>')
        itr.blank_adv();
      else
        itr.adv();
    }

    //html_filter->process_inplace(start, i);

    itr.next_line();

    prev_blank = blank_line;
  }
}

Block * MarkdownFilter::start_block(Iterator & itr) {
  inline_state->tag.reset();
  Block * nblk = NULL;
  (nblk = IndentedCodeBlock::start_block(prev_blank, itr))
    || (nblk = FencedCodeBlock::start_block(itr))
    || (nblk = BlockQuote::start_block(itr))
    || (nblk = ListItem::start_block(itr))
    || (nblk = SingleLineBlock::start_block(itr))
    || (nblk = start_html_block(inline_state->tag, itr));
  return nblk;
}

} // anon namespace

C_EXPORT IndividualFilter * new_aspell_markdown_filter() 
{
  return new MarkdownFilter();
}

