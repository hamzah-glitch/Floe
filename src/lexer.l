%option noyywrap nounput noinput yylineno
%x BLOCK_COMMENT

%{
#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "lexer.h"
#include "parser.hpp"

const char* floe_source_name = "<input>";
int floe_token_line = 1;
int floe_token_column = 1;
int floe_lex_error_count = 0;

namespace {

// Position of the next unread character.
int next_line = 1;
int next_column = 1;

// Where the currently open /* comment started, for error messages.
int comment_line = 0;
int comment_column = 0;

// Record where this token starts, then advance past its text.
// The position is also handed to Bison through yylloc.
void track_position(const char* text, int length) {
    floe_token_line = next_line;
    floe_token_column = next_column;
    for (int i = 0; i < length; ++i) {
        if (text[i] == '\n') {
            ++next_line;
            next_column = 1;
        } else if (text[i] != '\r') {
            ++next_column;
        }
    }
    yylloc.first_line = floe_token_line;
    yylloc.first_column = floe_token_column;
    yylloc.last_line = next_line;
    yylloc.last_column = next_column;
}

void lex_error(int line, int column, const char* format, ...) {
    std::fprintf(stderr, "%s:%d:%d: lexical error: ", floe_source_name, line, column);
    va_list args;
    va_start(args, format);
    std::vfprintf(stderr, format, args);
    va_end(args);
    std::fputc('\n', stderr);
    ++floe_lex_error_count;
}

// Strip the quotes from a string literal and decode its escape sequences.
// Returns false (after reporting) if an unknown escape is found.
bool decode_string(const char* text, int length, std::string& out) {
    bool ok = true;
    for (int i = 1; i < length - 1; ++i) {
        if (text[i] != '\\') {
            out += text[i];
            continue;
        }
        ++i;
        switch (text[i]) {
            case 'n': out += '\n'; break;
            case 't': out += '\t'; break;
            case 'r': out += '\r'; break;
            case '0': out += '\0'; break;
            case '\\': out += '\\'; break;
            case '"': out += '"'; break;
            default:
                lex_error(floe_token_line, floe_token_column + i - 1,
                          "unknown escape sequence '\\%c' in string literal", text[i]);
                ok = false;
                break;
        }
    }
    return ok;
}

}  // namespace

#define YY_USER_ACTION track_position(yytext, yyleng);
%}

DIGIT       [0-9]
IDENT_START [A-Za-z_]
IDENT_CONT  [A-Za-z_0-9]

%%

 /* ---- Whitespace and comments ---- */
[ \t\r\n]+                              ;
"//"[^\n]*                              ;
"/*"                                    {
                                            comment_line = floe_token_line;
                                            comment_column = floe_token_column;
                                            BEGIN(BLOCK_COMMENT);
                                        }
<BLOCK_COMMENT>"*/"                     BEGIN(INITIAL);
<BLOCK_COMMENT>[^*\n]+                  ;
<BLOCK_COMMENT>"*"                      ;
<BLOCK_COMMENT>\n                       ;
<BLOCK_COMMENT><<EOF>>                  {
                                            lex_error(comment_line, comment_column,
                                                      "unterminated block comment");
                                            BEGIN(INITIAL);
                                            yyterminate();
                                        }

 /* ---- Multi-character operators (must precede their one-character prefixes) ---- */
"|>"                                    return PIPE_OP;
"->"                                    return ARROW;
".."                                    return DOT_DOT;
"=="                                    return EQ;
"!="                                    return NE;
"<="                                    return LE;
">="                                    return GE;
"&&"                                    return AND;
"||"                                    return OR;

 /* ---- Keywords (must precede the identifier rule) ---- */
"let"                                   return LET;
"fn"                                    return FN;
"pipe"                                  return PIPE;
"if"                                    return IF;
"else"                                  return ELSE;
"for"                                   return FOR;
"while"                                 return WHILE;
"in"                                    return IN;
"true"                                  return TRUE;
"false"                                 return FALSE;
"int"                                   return INT_TYPE;
"float"                                 return FLOAT_TYPE;
"bool"                                  return BOOL_TYPE;
"string"                                return STRING_TYPE;

 /* ---- Literals ---- */
{DIGIT}+"."{DIGIT}+                     {
                                            yylval.float_value = std::strtod(yytext, nullptr);
                                            return FLOAT_LITERAL;
                                        }
{DIGIT}+                                {
                                            errno = 0;
                                            yylval.int_value = std::strtoll(yytext, nullptr, 10);
                                            if (errno == ERANGE) {
                                                lex_error(floe_token_line, floe_token_column,
                                                          "integer literal '%s' is too large", yytext);
                                                return INVALID_TOKEN;
                                            }
                                            return INT_LITERAL;
                                        }
{DIGIT}+("."{DIGIT}+)?{IDENT_START}{IDENT_CONT}* {
                                            lex_error(floe_token_line, floe_token_column,
                                                      "malformed number '%s'", yytext);
                                            return INVALID_TOKEN;
                                        }
\"([^\\\n\"]|\\.)*\"                    {
                                            std::string value;
                                            if (!decode_string(yytext, yyleng, value)) {
                                                return INVALID_TOKEN;
                                            }
                                            yylval.string_value = strdup(value.c_str());
                                            return STRING_LITERAL;
                                        }
\"([^\\\n\"]|\\.)*\\?                   {
                                            lex_error(floe_token_line, floe_token_column,
                                                      "unterminated string literal");
                                            return INVALID_TOKEN;
                                        }

 /* ---- Identifiers ---- */
{IDENT_START}{IDENT_CONT}*              {
                                            yylval.string_value = strdup(yytext);
                                            return IDENTIFIER;
                                        }

 /* ---- Single-character punctuation and operators ---- */
"{"|"}"|"["|"]"|"("|")"|";"|","|":"|"="|"+"|"-"|"*"|"/"|"%"|"<"|">"|"!" return yytext[0];

 /* ---- Errors ---- */
"|"                                     {
                                            lex_error(floe_token_line, floe_token_column,
                                                      "unexpected character '|' (did you mean '|>' or '||'?)");
                                            return INVALID_TOKEN;
                                        }
"&"                                     {
                                            lex_error(floe_token_line, floe_token_column,
                                                      "unexpected character '&' (did you mean '&&'?)");
                                            return INVALID_TOKEN;
                                        }
"."                                     {
                                            lex_error(floe_token_line, floe_token_column,
                                                      "unexpected character '.' (did you mean '..'?)");
                                            return INVALID_TOKEN;
                                        }
.                                       {
                                            const unsigned char c = static_cast<unsigned char>(yytext[0]);
                                            if (c >= 0x20 && c < 0x7f) {
                                                lex_error(floe_token_line, floe_token_column,
                                                          "unexpected character '%c'", c);
                                            } else {
                                                lex_error(floe_token_line, floe_token_column,
                                                          "unexpected byte 0x%02X", c);
                                            }
                                            return INVALID_TOKEN;
                                        }

%%
