#include "settings.h"

#include "sgml.hpp"
#include "indiv_filter.hpp"
#include "iostream.hpp"

using namespace aspell_filters;

namespace {

struct Iterator;

struct Block {
  Block * next;
  Block() : next() {}
  enum KeepOpenState {NEVER, MAYBE, YES};
  virtual KeepOpenState keep_open(Iterator &) = 0;
  virtual bool blank_rest() const = 0;
  virtual void dump() const = 0;
  virtual bool leaf() const = 0;
  virtual ~Block() {}
};

struct DocRoot : Block {
  KeepOpenState keep_open(Iterator &) {return YES;}
  bool blank_rest() const {return false;}
  void dump() const {CERR.printf("DocRoot\n");}
  bool leaf() const {return false;}
};

class MarkdownFilter : public IndividualFilter {
public:
  MarkdownFilter() : root(), back(&root), prev_blank(true), inline_code(0) {
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

  DocRoot root;
  Block * back;
  bool prev_blank;
  int inline_code;

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
}

//
// Markdown blocks
// 

struct BlockQuote : Block {
  static BlockQuote * start_block(Iterator & itr) {
    if (*itr == '>') {
      itr.blank_adv();
      return new BlockQuote();
    }
    return NULL;
  }
  KeepOpenState keep_open(Iterator & itr) {
    if (*itr == '>') {
      itr.blank_adv();
      return YES;
    } else if (itr.eol()) {
      return NEVER;
    }
    return MAYBE;
  }
  bool blank_rest() const {
    return false;
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
      itr.blank_adv(width);
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
  KeepOpenState keep_open(Iterator & itr) {
    if (!itr.eol() && itr.indent >= indent) {
      itr.indent -= indent;
      return YES;
    }
    return MAYBE;
  }
  bool blank_rest() const {
    return false;
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
  KeepOpenState keep_open(Iterator & itr) {
    if (itr.indent >= 4) {
      itr.indent -= 4;
      return YES;
    } else if (itr.eol()) {
      return YES;
    }
    return NEVER;
  }
  bool blank_rest() const {
    return true;
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
  KeepOpenState keep_open(Iterator & itr) {
    if (*itr == '`' || *itr == '~') {
      char delem = *itr;
      int i = 1;
      while (itr[i] == delem)
        ++i;
      if (i < delem_len) return MAYBE;
      itr.blank_adv(i);
      if (!itr.eol()) return MAYBE;
      return NEVER;
    }
    return MAYBE;
  }
  bool blank_rest() const {
    return true;
  }
  void dump() const {CERR.printf("FencedCodeBlock: `%c` %d\n", delem, delem_len);}
  bool leaf() const {return true;}
};

// tests if we start some other block and need and need to close unmatched blocks
bool single_line_block_proc(Iterator & itr) {
#if 0
  if (indent >= 4)
    return false;
  switch (*itr) {
  case '-': case '_': case '*':
    Iterator i = itr;
    i.adv();
    while (*i == *itr)
      i.adv();
    if (i.eol()) {
      itr = i;
      return true;
    }
  case '-': case '=':
    Iterator i = itr;
    i.inc();
    while (*i == *itr)
      i.inc();
    i.eat_space();
    if (i.eol()) {
      itr = i;
      return true;
    }
  case '#':
    return true;
    break;
  case '[':
    Iterator i = itr;
    i.adv();
    if (*i == ']') break;
    while (*i != ']') {
      i.adv();
    }
    i.inc();
    if (*i != ':') break;
    break;
  }
#endif
  return false;
}

//
// MarkdownFilter implementation
//

PosibErr<bool> MarkdownFilter::setup(Config * cfg) {
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

struct NewBlock {
  Block * blk; // empty if a single line block
  bool yes;
  NewBlock() : blk(NULL), yes(false) {}
  NewBlock(Block * blk, bool yes) : blk(blk), yes(yes) {}
};

NewBlock new_block(bool prev_blank, Iterator & itr) {
  Block * nblk = NULL;
  bool yes = (nblk = IndentedCodeBlock::start_block(prev_blank, itr))
    || (nblk = FencedCodeBlock::start_block(itr))
    || (nblk = BlockQuote::start_block(itr))
    || (nblk = ListItem::start_block(itr))
    || single_line_block_proc(itr);
  return NewBlock(nblk, yes);
}

int handle_inline_code(int marker_len, Iterator & itr) {
  while (!itr.eol()) {
    if (*itr == '`') {
      int i = 1;
      while (i < marker_len && itr[i] == '`')
        ++i;
      if (i == marker_len) {
        itr.blank_adv(i);
        return 0;
      }
    }
    itr.blank_adv();
  }
  return marker_len;
}

void MarkdownFilter::process(FilterChar * & start, FilterChar * & stop) {
  Iterator itr(start,stop);
  bool blank_line = false;
  while (!itr.eos()) {
    CERR.printf("*** inline_code = %d\n", inline_code);
    if (inline_code > 0) {
      inline_code = handle_inline_code(inline_code, itr);
    } else {
      itr.eat_space();
      
      Block * blk = &root;
      Block::KeepOpenState keep_open;
      for (; blk; blk = blk->next) {
        keep_open = blk->keep_open(itr);
        if (keep_open != Block::YES)
          break;
      }
      
      blank_line = itr.eol();
      NewBlock new_blk = blank_line || (keep_open == Block::YES && back->leaf())
        ? NewBlock()
        : new_block(prev_blank, itr);
      
      if (new_blk.yes || keep_open == Block::NEVER || (prev_blank && !blank_line)) {
        CERR.printf("*** kill\n");
        kill(blk);
      }
      if (new_blk.blk) {
        prev_blank = true;
      CERR.printf("*** new block\n");
      add(new_blk.blk);
      }
      
      // note: single line blocks are always leafs
      while (new_blk.blk && !new_blk.blk->leaf()) {
        new_blk = new_block(prev_blank, itr);
        if (new_blk.blk) {
          CERR.printf("*** new block\n");
          add(new_blk.blk);
        }
      }
    
      dump();
      
      if (back->blank_rest()) {
        itr.blank_rest();
      }
    }

    // now process line, mainly blank inline code and handle html tags
    while (!itr.eol()) {
      if (*itr == '`') {
        int i = 1;
        while (itr[i] == '`')
          ++i;
        itr.blank_adv(i);
        inline_code = handle_inline_code(i, itr);
      }
      itr.adv();
    }

    //html_filter->process_inplace(start, i);

    itr.next_line();

    prev_blank = blank_line;
  }
}

} // anon namespace

C_EXPORT IndividualFilter * new_aspell_markdown_filter() 
{
  return new MarkdownFilter();
}

