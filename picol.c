/* Tcl in ~ 500 lines of code.
 *
 * Copyright (c) 2007-2016, Salvatore Sanfilippo <antirez at gmail dot com>
 *                    2022, Tristan Styles
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *   * Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <ctype.h>

enum {PICOL_OK, PICOL_ERR, PICOL_RETURN, PICOL_BREAK, PICOL_CONTINUE};
enum {PT_ESC,PT_STR,PT_CMD,PT_VAR,PT_SEP,PT_EOL,PT_EOF};

struct picolParser {
	char *text, *pos, *start, *end;
	int len, type, insidequote;
};

struct picolVar {
	char *name, *val;
	struct picolVar *next;
};

struct picolInterp {
	int level; /* Level of nesting */
	struct picolCallFrame *callframe;
	struct picolCmd *commands;
	char *result;
};

typedef int (*picolCmdFunc)(struct picolInterp *i, int argc, char **argv, void *privdata);

struct picolCmd {
	char *name;
	picolCmdFunc func;
	void *privdata;
	struct picolCmd *next;
};

struct picolCallFrame {
	struct picolVar *vars;
	struct picolCallFrame *parent; /* parent is NULL at top level */
};

static char *picolGets(FILE *in, int end) {
	char *buf = malloc(1);
	for (int n=0, z=0, c; (buf[n]='\0') || (c=fgetc(in))!=end; buf[n++]=c)
		if (n==z) buf = realloc(buf,(z=(z+1)+(z>>1))+1);
	return buf;
}

static void picolSetResult(struct picolInterp *i, char *s) {
	free(i->result);
	i->result = strdup(s);
}

static int picolErr(struct picolInterp *i, char const *f, ...) {
	va_list v1, v2; va_start(v1,f), va_copy(v2,v1);
	size_t n = vsnprintf(NULL,0,f,v1);
	free(i->result);
	i->result = malloc(n+1);
	vsnprintf(i->result,n+1,f,v2);
	va_end(v2), va_end(v1);
	return PICOL_ERR;
}

static void picolInitParser(struct picolParser *p, char *text) {
	p->text = p->pos = p->start = p->end = text;
	p->len = strlen(text);
	p->insidequote = 0;
	p->type = PT_EOL;
}

static int picolParseSep(struct picolParser *p, int eol) {
	for (p->start = p->pos; !isgraph(*p->pos) || (eol && *p->pos == eol); p->pos++, p->len--);
	p->end = p->pos-1;
	p->type = eol ? PT_EOL : PT_SEP;
	return PICOL_OK;
}

static int picolParseCommand(struct picolParser *p) {
	p->start = ++p->pos; p->len--; /* skip the initial opening bracket */
	for (int level = 1, blevel = 0; p->len > 0; p->pos++, p->len--)
		if (*p->pos == '\\') {
			if (p->len > 1) p->pos++, p->len--;
		} else if (*p->pos == '[') {
			if (blevel == 0) level++;
		} else if (*p->pos == ']') {
			if (blevel == 0 && !--level) break;
		} else if (*p->pos == '{') {
			blevel++;
		} else if (*p->pos == '}') {
			if (blevel != 0) blevel--;
		}
	p->end = p->pos-1;
	p->type = PT_CMD;
	if (*p->pos == ']') p->pos++, p->len--; /* Skip final closed bracket */
	return PICOL_OK;
}

static int picolParseVar(struct picolParser *p) {
	p->start = ++p->pos; p->len--; /* skip the $ */
	for(; isalnum(*p->pos) || *p->pos == '_'; p->pos++, p->len--);
	if (p->start == p->pos) { /* It's just a single char string "$" */
		p->start = p->end = p->pos-1;
		p->type = PT_STR;
	} else {
		p->end = p->pos-1;
		p->type = PT_VAR;
	}
	return PICOL_OK;
}

static int picolParseBrace(struct picolParser *p) {
	p->start = ++p->pos; p->len--; /* skip the initial opening brace */
	for (int level = 1; p->len > 0; p->pos++, p->len--)
		if (*p->pos == '{') level++;
		else if (p->len >= 2 && *p->pos == '\\') p->pos++, p->len--;
		else if (*p->pos == '}' && !--level) break;
	p->end = p->pos-1;
	if (p->len > 0) p->pos++, p->len--; /* Skip final closed brace */
	p->type = PT_STR;
	return PICOL_OK;
}

static int picolParseString(struct picolParser *p) {
	int newword = (p->type == PT_SEP || p->type == PT_EOL || p->type == PT_STR);
	if (newword && *p->pos == '{') return picolParseBrace(p);
	if (newword && *p->pos == '"') {
		p->insidequote = 1;
		p->pos++, p->len--;
	}
	for (p->start = p->pos; p->len > 0; p->pos++, p->len--)
		switch (*p->pos) {
		default:
			if ((!isgraph(*p->pos) || *p->pos == ';') && !p->insidequote) {
		case '$': case '[':
				goto end;
			}
			break;
		case '"':
			if (!p->insidequote) break;
			p->end = p->pos-1;
			p->type = PT_ESC;
			p->pos++, p->len--;
			p->insidequote = 0;
			return PICOL_OK;
		case '\\':
			if (p->len >= 2) p->pos++, p->len--;
		}
end:
	p->end = p->pos-1;
	p->type = PT_ESC;
	return PICOL_OK;
}

static int picolParseComment(struct picolParser *p) {
	for(; p->len && *p->pos != '\n'; p->pos++, p->len--);
	return PICOL_OK;
}

static int picolGetToken(struct picolParser *p) {
	while (p->len > 0)
		switch (*p->pos) {
		default:
			if (isgraph(*p->pos) || p->insidequote) return picolParseString(p);
			return picolParseSep(p, 0);
		case '\n': case ';':
			if (p->insidequote) return picolParseString(p);
			return picolParseSep(p, ';');
		case '[': return picolParseCommand(p);
		case '$': return picolParseVar(p);
		case '#':
			if (p->type != PT_EOL) return picolParseString(p);
			picolParseComment(p);
		}
	p->type = (p->type != PT_EOL && p->type != PT_EOF) ? PT_EOL : PT_EOF;
	return PICOL_OK;
}

static void picolInitInterp(struct picolInterp *i) {
	i->level = 0;
	i->callframe = malloc(sizeof(struct picolCallFrame));
	i->callframe->vars = NULL;
	i->callframe->parent = NULL;
	i->commands = NULL;
	i->result = strdup("");
}

static struct picolVar *picolGetVar(struct picolInterp *i, char *name) {
	for (struct picolVar *v = i->callframe->vars; v != NULL; v = v->next)
		if (strcmp(v->name,name) == 0) return v;
	return NULL;
}

static int picolSetVar(struct picolInterp *i, char *name, char *val) {
	struct picolVar *v = picolGetVar(i,name);
	if (v) free(v->val);
	else {
		v = malloc(sizeof(*v));
		v->name = strdup(name);
		v->next = i->callframe->vars;
		i->callframe->vars = v;
	}
	v->val = strdup(val);
	return PICOL_OK;
}

static struct picolCmd *picolGetCommand(struct picolInterp *i, char *name) {
	for (struct picolCmd *c = i->commands; c != NULL; c = c->next)
		if (strcmp(c->name,name) == 0) return c;
	return NULL;
}

static int picolRegisterCommand(struct picolInterp *i, char *name, picolCmdFunc f, void *privdata) {
	struct picolCmd *c = picolGetCommand(i,name);
	if (c) return picolErr(i,"Command '%s' already defined",name);
	c = malloc(sizeof(*c));
	c->name = strdup(name);
	c->func = f;
	c->privdata = privdata;
	c->next = i->commands;
	i->commands = c;
	return PICOL_OK;
}

static int toxdigit(int c) { return isalpha(c) ? (10+(toupper(c)-'A')) : (c-'0'); }

static int picolEscape(char *b, int n) {
	char *s = strchr(b, '\\');
	if (s) for (char *t = s; *s; ) {
		if (*s == '\\')
			switch (*(s+1)) {
			default :
				s++, n--;
				if (isgraph(*s)) break;
				for (s++, n--; n > 0 && !isgraph(*s); s++, n--);
				continue;
			case 'X': case 'x':
				s+=2, n-=2;
				if (isxdigit(*s) && isxdigit(*(s+1))) {
					*t++ = (toxdigit(*s) << 4) | toxdigit(*(s+1));
					s+=2, n-=2;
				} else if (isxdigit(*s)) {
					*t++ = toxdigit(isalpha(*s));
					s++, n--;
				}
				continue;
			case 'n': s+=2, n--, *t++ = '\n'; continue;
			case 'r': s+=2, n--, *t++ = '\r'; continue;
			case 't': s+=2, n--, *t++ = '\t'; continue;
			case'\0': s++, n--; continue;
			}
		*t++ = *s++;
	}
	b[n] = '\0';
	return n;
}

static int picolEval(struct picolInterp *i, char *s) {
	struct picolParser p;
	int retcode = PICOL_OK, argc = 0, j;
	char **argv = NULL;
	picolSetResult(i,"");
	picolInitParser(&p,s);
	for (int prevtype = p.type; picolGetToken(&p) == PICOL_OK; prevtype = p.type) {
		if (p.type == PT_EOF) break;
		int tlen = p.end-p.start+1;
		if (tlen < 0) tlen = 0;
		char *t = memcpy(malloc(tlen+1), p.start, tlen);
		t[tlen] = '\0';
		if (p.type == PT_VAR) {
			struct picolVar *v = picolGetVar(i,t);
			if (!v) {
				retcode = picolErr(i,"No such variable '%s'",t);
				free(t);
				break;
			}
			free(t);
			t = strdup(v->val);
			tlen = strlen(t);
		} else if (p.type == PT_CMD) {
			retcode = picolEval(i,t);
			free(t);
			if (retcode != PICOL_OK) break;
			t = strdup(i->result);
			tlen = strlen(t);
		} else if (p.type == PT_ESC) {
			tlen = picolEscape(t, tlen);
		} else if (p.type == PT_SEP) {
			free(t);
			continue;
		} else if (p.type == PT_EOL) {
			struct picolCmd *c;
			free(t);
			if (argc) { /* We have a complete command + args. Call it! */
				if ((c = picolGetCommand(i,argv[0])) == NULL) {
					retcode = picolErr(i,"No such command '%s'",argv[0]);
					break;
				}
				retcode = c->func(i,argc,argv,c->privdata);
				if (retcode != PICOL_OK) break;
			}
			/* Prepare for the next command */
			for (j = 0; j < argc; j++) free(argv[j]);
			free(argv);
			argv = NULL;
			argc = 0;
			continue;
		}
		/* We have a new token, append to the previous or as new arg? */
		if (prevtype == PT_SEP || prevtype == PT_EOL) {
			argv = realloc(argv, sizeof(char*)*(argc+1));
			argv[argc] = t;
			argc++;
		} else { /* Interpolation */
			int oldlen = strlen(argv[argc-1]);
			argv[argc-1] = realloc(argv[argc-1], oldlen+tlen+1);
			memcpy(argv[argc-1]+oldlen, t, tlen);
			argv[argc-1][oldlen+tlen]='\0';
			free(t);
		}
	}
	for (j = 0; j < argc; j++) free(argv[j]);
	free(argv);
	return retcode;
}

static int picolArityErr(struct picolInterp *i, char *name) {
	return picolErr(i,"Wrong number of args for %s",name);
}

static int picolCommandMath(struct picolInterp *i, int argc, char **argv, void *pd) {
	char buf[(sizeof(int)*CHAR_BIT)/3];
	if (argc != 3) return picolArityErr(i,argv[0]);
	int o = argv[0][0]^argv[0][1]<<8, a = atoi(argv[1]), b = atoi(argv[2]), c = 0;
	/**/ if (o ==  '+') c = a + b;
	else if (o ==  '-') c = a - b;
	else if (o ==  '*') c = a * b;
	else if (o ==  '/') c = a / b;
	else if (o ==  '>') c = a > b;
	else if (o == ('>'^'='<<8)) c = a >= b;
	else if (o ==  '<') c = a < b;
	else if (o == ('<'^'='<<8)) c = a <= b;
	else if (o == ('='^'='<<8)) c = a == b;
	else if (o == ('!'^'='<<8)) c = a != b;
	snprintf(buf,sizeof(buf),"%d",c);
	picolSetResult(i,buf);
	return PICOL_OK;
}

static int picolCommandSet(struct picolInterp *i, int argc, char **argv, void *pd) {
	if (argc != 3) return picolArityErr(i,argv[0]);
	picolSetVar(i,argv[1],argv[2]);
	picolSetResult(i,argv[2]);
	return PICOL_OK;
}

static int picolCommandPuts(struct picolInterp *i, int argc, char **argv, void *pd) {
    if (argc != 2) return picolArityErr(i,argv[0]);
    puts(argv[1]);
	return PICOL_OK;
}

static int picolCommandIf(struct picolInterp *i, int argc, char **argv, void *pd) {
	if (argc != 3 && argc != 5) return picolArityErr(i,argv[0]);
	for (int retcode = picolEval(i,argv[1]); retcode != PICOL_OK; ) return retcode;
	if (atoi(i->result)) return picolEval(i,argv[2]);
	if (argc == 5) return picolEval(i,argv[4]);
	return PICOL_OK;
}

static int picolCommandWhile(struct picolInterp *i, int argc, char **argv, void *pd) {
	for (int retcode; argc == 3; )
		if ((retcode = picolEval(i,argv[1])) != PICOL_OK) return retcode;
		else if (atoi(i->result) == 0) return PICOL_OK;
		else if ((retcode = picolEval(i,argv[2])) == PICOL_CONTINUE) continue;
		else if (retcode == PICOL_OK) continue;
		else if (retcode != PICOL_BREAK) return retcode;
	return picolArityErr(i,argv[0]);
}

static int picolCommandRetCodes(struct picolInterp *i, int argc, char **argv, void *pd) {
	if (argc != 1) return picolArityErr(i,argv[0]);
	if (strcmp(argv[0],"break") == 0) return PICOL_BREAK;
	if (strcmp(argv[0],"continue") == 0) return PICOL_CONTINUE;
	return PICOL_OK;
}

static void picolDropCallFrame(struct picolInterp *i) {
	struct picolCallFrame *cf = i->callframe;
	for (struct picolVar *v = cf->vars, *t; v != NULL; v = t) {
		t = v->next;
		free(v->name);
		free(v->val);
		free(v);
	}
	i->callframe = cf->parent;
	free(cf);
}

static int picolCommandCallProc(struct picolInterp *i, int argc, char **argv, void *pd) {
	char **x=pd, *alist=x[0], *body=x[1], *tofree=strdup(alist);
	struct picolCallFrame *cf = malloc(sizeof(*cf));
	int arity = 0, done = 0, errcode = PICOL_OK;
	cf->vars = NULL;
	cf->parent = i->callframe;
	i->callframe = cf;
	for (char *s = tofree, *start; !done; s++) {
		for (start = s; *s != ' ' && *s != '\0'; s++);
		if (*s != '\0' && s == start) continue;
		if (s == start) break;
		if (*s == '\0') done=1; else *s = '\0';
		if (++arity > argc-1) goto arityerr;
		picolSetVar(i,start,argv[arity]);
	}
	free(tofree);
	if (arity != argc-1) goto arityerr;
	errcode = picolEval(i,body);
	if (errcode == PICOL_RETURN) errcode = PICOL_OK;
	picolDropCallFrame(i); /* remove the called proc callframe */
	return errcode;
arityerr:
	picolDropCallFrame(i); /* remove the called proc callframe */
	return picolErr(i,"Proc '%s' called with wrong arg num",argv[0]);
}

static int picolCommandProc(struct picolInterp *i, int argc, char **argv, void *pd) {
	if (argc != 4) return picolArityErr(i,argv[0]);
	char **procdata = malloc(sizeof(char*)*2);
	procdata[0] = strdup(argv[2]); /* arguments list */
	procdata[1] = strdup(argv[3]); /* procedure body */
	return picolRegisterCommand(i,argv[1],picolCommandCallProc,procdata);
}

static int picolCommandReturn(struct picolInterp *i, int argc, char **argv, void *pd) {
	if (argc != 1 && argc != 2) return picolArityErr(i,argv[0]);
	picolSetResult(i, (argc == 2) ? argv[1] : "");
	return PICOL_RETURN;
}

static void picolRegisterCoreCommands(struct picolInterp *i) {
	char *name[] = {"+","-","*","/",">",">=","<","<=","==","!="};
	for (int j = 0; j < (int)(sizeof(name)/sizeof(char*)); j++)
		picolRegisterCommand(i,name[j],picolCommandMath,NULL);
	picolRegisterCommand(i,"set",picolCommandSet,NULL);
	picolRegisterCommand(i,"puts",picolCommandPuts,NULL);
	picolRegisterCommand(i,"if",picolCommandIf,NULL);
	picolRegisterCommand(i,"while",picolCommandWhile,NULL);
	picolRegisterCommand(i,"break",picolCommandRetCodes,NULL);
	picolRegisterCommand(i,"continue",picolCommandRetCodes,NULL);
	picolRegisterCommand(i,"proc",picolCommandProc,NULL);
	picolRegisterCommand(i,"return",picolCommandReturn,NULL);
}

int main(int argc, char **argv) {
	char *buf;
	struct picolInterp interp;
	picolInitInterp(&interp);
	picolRegisterCoreCommands(&interp);
	for (int retcode; argc == 1; free(buf)) {
		printf("picol> "), fflush(stdout);
		buf = picolGets(stdin,'\n');
		if (strcmp(buf,"quit") == 0) return EXIT_SUCCESS;
		retcode = picolEval(&interp,buf);
		if (interp.result[0] != '\0') printf("[%d] %s\n", retcode, interp.result);
	}
	for (FILE *fp; (argc>1) && (fp=fopen(argv[1],"r")); free(buf), argc--, argv++) {
		buf = picolGets(fp,EOF), fclose(fp);
		if (picolEval(&interp,buf) != PICOL_OK) puts(interp.result);
	}
	if (argc < 2) return EXIT_SUCCESS;
	perror(argv[1]);
	return EXIT_FAILURE;
}
