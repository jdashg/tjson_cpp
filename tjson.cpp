#include "tjson.h"

#include <algorithm>
#include <cstdio>
#include <locale>
#include <ostream>
#include <regex>
#include <sstream>

namespace tjson {

/*static*/ const Val Val::INVALID;

// -

std::string
escape(const std::string& in)
{
   std::string out;
   out.reserve(in.size() + 2); // Only reserve the required quotes.

   out += '"';
   for (const auto c : in) {
      if (c == '"' || c == '\\') {
         out += '\\';
      }
      out += c;
   }
   out += '"';
   return out;
}

bool
unescape(const std::string& in, std::string* const out)
{
   if (in.size() < 2)
      return false;
   if (in[0] != '"' || in[in.size()-1] != '"')
      return false;

   std::string wip;
   wip.reserve(in.size() - 2);

   bool in_escape = false;
   auto itr = in.c_str() + 1;
   const auto end = in.c_str() + in.size() - 1;
   for (; itr != end; ++itr) {
      const auto& c = *itr;
      if (!in_escape && c == '\\') {
         in_escape = true;
      } else {
         in_escape = false;
         wip += c;
      }
   }
   if (in_escape)
      return false;

   *out = std::move(wip);
   return true;
}

// -

struct Token final
{
   enum class Type : uint8_t {
      MALFORMED = '?',
      WHITESPACE = '_',
      STRING = '"',
      WORD = 'a',
      SYMBOL = '$',
   };

   const char* begin;
   const char* end;
   uint64_t line_num;
   uint64_t line_pos;
   Type type;

   bool operator==(const char* const r) const {
      const auto len = strlen(r);
      if (len != end - begin)
         return false;
      return std::equal(begin, end, r);
   }

   std::string str() const { return std::string(begin, end); }
};

const std::regex RE_WHITESPACE(R"([ \t\n\r]+)");
const std::regex RE_STRING(R"_("(:?[^"\\]*(:?\\.)*)*")_");
const std::regex RE_WORD(R"([A-Za-z0-9_+\-.]+)");
const std::regex RE_SYMBOL(R"([{:,}\[\]])");

static bool
next_token_from_regex(const std::regex& regex, Token* const out)
{
   std::cmatch res;
   if (!std::regex_search(out->begin, out->end, res, regex,
                          std::regex_constants::match_continuous))
   {
      return false;
   }
   out->end = out->begin + res.length();
   return true;
}

class TokenGen final
{
   Token meta_token_;

public:
   TokenGen(const char* const begin, const char* const end)
      : meta_token_{begin, end, 1, 1, Token::Type::MALFORMED}
   { }

   Token Next() {
      auto ret = meta_token_;
      if (next_token_from_regex(RE_WHITESPACE, &ret)) {
         ret.type = Token::Type::WHITESPACE;
      } else if (next_token_from_regex(RE_STRING, &ret)) {
         ret.type = Token::Type::STRING;
      } else if (next_token_from_regex(RE_WORD, &ret)) {
         ret.type = Token::Type::WORD;
      } else if (next_token_from_regex(RE_SYMBOL, &ret)) {
         ret.type = Token::Type::SYMBOL;
      }

      meta_token_.begin = ret.end;
      for (auto itr = ret.begin; itr != ret.end; ++itr) {
         if (*itr == '\n') {
            meta_token_.line_num += 1;
            meta_token_.line_pos = 0;
         }
         meta_token_.line_pos += 1;
      }
      return ret;
   }

   Token NextNonWS() {
      while (true) {
         const auto ret = Next();
         if (ret.type != Token::Type::WHITESPACE) {

//#define SPEW_TOKENS
#ifdef SPEW_TOKENS
            fprintf(stderr, "%c @ L%llu:%llu: %s\n\n", int(ret.type), ret.line_num,
                    ret.line_pos, ret.str().c_str());
#endif
            return ret;
         }
      }
   }
};

// -

std::unique_ptr<Val>
read(const char* const begin, const char* const end,
     std::string* const out_err)
{
   TokenGen tok_gen(begin, end);
   auto ret = read(&tok_gen, out_err);
   return std::move(ret);
}

std::unique_ptr<Val>
read(TokenGen* const tok_gen,
     std::string* const out_err)
{
   const auto fn_err = [&](const Token& tok, const char* const expected) {
      auto str = tok.str();
      if (str.length() > 20) {
         str.resize(20);
      }

      std::ostringstream err;
      err << "Error: L" << tok.line_num << ":" << tok.line_pos << ": Expected "
          << expected << ", got: \"" << str << "\".";
      *out_err = err.str();
   };

   const auto fn_is_expected = [&](const Token& tok,
                                   const char* const expected_str = nullptr)
   {
      std::string expl_expected_str;
      const char* expl_expected;
      if (expected_str) {
         if (tok == expected_str)
            return true;
         expl_expected_str = std::string("\"") + expected_str + "\"";
         expl_expected = expl_expected_str.c_str();
      } else {
         if (tok.type != Token::Type::MALFORMED)
            return true;
         expl_expected = "!MALFORMED";
      }
      fn_err(tok, expl_expected);
      return false;
   };


   auto ret = std::unique_ptr<Val>(new Val);
   auto& cur = *ret;

   const auto tok = tok_gen->NextNonWS();
   if (tok == "{") {
      cur.set_dict();

      auto peek_gen = *tok_gen;
      const auto peek = peek_gen.NextNonWS();
      if (peek == "}") {
         *tok_gen = peek_gen;
      } else {
         while (true) {
            const auto k = tok_gen->NextNonWS();
            if (k.type != Token::Type::STRING) {
               fn_err(k, "STRING");
               return nullptr;
            }

            const auto colon = tok_gen->NextNonWS();
            if (!fn_is_expected(colon, ":"))
               return nullptr;

            auto v = read(tok_gen, out_err);
            if (!v)
               return nullptr;

            std::string key_name;
            (void)unescape(k.str(), &key_name);
            cur[key_name] = std::move(v); // Overwrite.

            const auto comma = tok_gen->NextNonWS();
            if (comma == "}")
               break;
            if (!fn_is_expected(comma, ","))
               return nullptr;
            continue;
         }
      }
      return ret;
   }

   if (tok == "[") {
      cur.set_list();

      auto peek_gen = *tok_gen;
      const auto peek = peek_gen.NextNonWS();
      if (peek == "]") {
         *tok_gen = peek_gen;
      } else {
         size_t i = 0;
         while (true) {
            auto v = read(tok_gen, out_err);
            if (!v)
               return nullptr;

            cur[i] = std::move(v);
            i += 1;

            const auto comma = tok_gen->NextNonWS();
            if (comma == "]")
               break;
            if (!fn_is_expected(comma, ","))
               return nullptr;
            continue;
         }
      }
      return ret;
   }

   if (!fn_is_expected(tok))
      return nullptr;

   cur.val() = tok.str();
   return ret;
}

void
write(const Val& root, std::ostream* const out, const std::string& indent)
{
   if (root.is_dict()) {
      const auto& d = root.dict();
      *out << "{";
      if (!d.size()) {
         *out << "}";
         return;
      }
      const auto indent_plus = indent + "   ";
      bool needsComma = false;
      for (const auto& kv : d) {
         if (needsComma) {
            *out << ",";
         }

         *out << "\n" << indent_plus << escape(kv.first) << ": ";
         kv.second->write(out, indent_plus);

         needsComma = true;
      }
      *out << "\n" << indent << "}";
      return;
   }

   if (root.is_list()) {
      const auto& l = root.list();
      *out << "[";
      if (!l.size()) {
         *out << "]";
         return;
      }
      const auto indent_plus = indent + "   ";
      bool needsComma = false;
      for (const auto& v : l) {
         if (needsComma) {
            *out << ",";
         }

         *out << "\n" << indent_plus;
         v->write(out, indent_plus);

         needsComma = true;
      }
      *out << "\n" << indent << "]";
      return;
   }

   *out << root.val();
}

// -

bool
Val::as_number(double* const out) const
{
   auto stream = std::istringstream(val_);

   // Set the C locale, otherwise 1.5 parses as 1.0 in decimal-comma locales!
   const auto locale = std::locale::classic();
   stream.imbue(locale);

   double ret;
   stream >> ret;
   if (stream.fail())
      return false;

   *out = ret;
   return true;
}

void
Val::val(const double x)
{
   auto stream = std::ostringstream();

   const auto locale = std::locale::classic();
   stream.imbue(locale);

   stream << x;

   val() = stream.str();
}

// -

void
Val::reset()
{
   dict_.clear();
   is_dict_ = false;

   list_.clear();
   is_list_ = false;

   val_ = "";
}

// -

const Val&
Val::operator[](const std::string& x) const
{
   const auto itr = dict_.find(x);
   if (itr == dict_.end())
      return INVALID;
   return *(itr->second.get());
}

std::unique_ptr<Val>&
Val::operator[](const std::string& x)
{
   set_dict();
   auto& val = dict_[x];
   val.reset(new Val);
   return val;
}

// -

const Val&
Val::operator[](const size_t i) const
{
   if (i >= list_.size())
      return INVALID;
   return *(list_[i].get());
}

std::unique_ptr<Val>&
Val::operator[](const size_t i)
{
   set_list();
   while (i >= list_.size()) {
      list_.push_back(std::unique_ptr<Val>(new Val));
   }
   return list_[i];
}

} // namespace tjson
