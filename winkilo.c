/*** Includes ***/
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <ctype.h>
#include <errno.h>
#include <time.h>
#include <string.h>
#include <sys/types.h>
#include <stdbool.h>

/*** Defines ***/
#define ESC "\x1b"
#define CSI "\x1b["
#define BELL "\x07"
#define BUFF_MAX 1024
#define MAXINREC 128
#define CTRL_KEY(k) (k & 0x1f)
#define KILO_VERSION "0.0.1"
#define KILO_TAB_STOP 4
#define KILO_QUIT_TIMES 3
#define KILO_TITLE "WinKilo - v" KILO_VERSION

enum EditorKey {
	BACKSPACE = 127,
	ARROW_LEFT = 1000,
	ARROW_RIGHT,
	ARROW_UP,
	ARROW_DOWN,
	DEL_KEY,
	HOME_KEY,
	END_KEY,
	PAGE_UP,
	PAGE_DOWN
};

enum EditorHighlight {
	HL_NORMAL = 0,
	HL_COMMENT,
	HL_MLCOMMENT,
	HL_KEYWORD1,
	HL_KEYWORD2,
	HL_STRING,
	HL_NUMBER,
	HL_MATCH
};

#define HL_HIGHLIGHT_NUMBERS (1<<0)
#define HL_HIGHLIGHT_STRINGS (1<<1)

/*** Data ***/
struct EditorSyntax {
	char *filetype;
	char **filematch;
	char **keywords;
	char *singleline_comment_start;
	char *multiline_comment_start;
	char *multiline_comment_end;
	int flags;
};

typedef struct line {
	int idx;
	size_t size;
	size_t rsize;
	char *bytes;
	char *render;
	unsigned char *hl;
	int hl_open_comment;
} line_t;

struct EditorConfig {
	DWORD 	dwOutMode;	// Orignial stdout mode.
	DWORD	dwInMode;	// Original stdin mode.
	HANDLE 	hStdin;		// Stdin handle.
	HANDLE	hStdout;	// Stdout handle.
	COORD 	bufSize;	// Screen buffer size.
	COORD	cursor;		// Current cursor position.
	COORD	rcursor;	// Render cursor position.
	COORD	offset;		// Editor offset.
	int		rx;			// Render X position.
	line_t	*line;		// Text lines.
	size_t	linesnum;	// Number of lines.
	int		dirty;
	char	*filename;
	char	statusmsg[80];
	time_t	statusmsg_time;
	struct	EditorSyntax *syntax;
};

struct EditorConfig E;

/*** Filetypes ***/
char *C_HL_extensions[] = { ".c", ".h", ".cpp", NULL };
char *C_HL_keywords[] = {
	"switch", "if", "while", "for", "break", "continue", "return", "else", 
	"struct", "union", "typedef", "static", "enum", "class", "case",

	"int|", "long|", "double|", "float|", "char|", "unsigned|", "signed|",
	"void|", NULL
};

struct EditorSyntax HLDB[] = {
	{
		"c",
		C_HL_extensions,
		C_HL_keywords,
		"//", "/*", "*/",
		HL_HIGHLIGHT_NUMBERS | HL_HIGHLIGHT_STRINGS
	},
};

#define HLDB_ENTRIES (sizeof(HLDB) / sizeof(HLDB[0]))

/*** Prototypes ***/
void EditorSetStatusMessage(const char *fmt, ...);
void EditorRefreshScreen();
char *EditorPrompt(char *prompt, void (*callback)(char *, int));

/*** Syntax Highlighting ***/
int is_separator(int c)
{
	return isspace(c) || c == '\0' || strchr(",.()+-/*=~%<>[];", c) != NULL;
}

void EditorUpdateSyntax(line_t *line)
{
	line->hl = realloc(line->hl, line->rsize);
	memset(line->hl, HL_NORMAL, line->rsize);

	if (E.syntax == NULL) return;

	char **keywords = E.syntax->keywords;

	char *scs = E.syntax->singleline_comment_start;
	char *mcs = E.syntax->multiline_comment_start;
	char *mce = E.syntax->multiline_comment_end;

	int scs_len = scs ? strlen(scs) : 0;
	int mcs_len = scs ? strlen(mcs) : 0;
	int mce_len = scs ? strlen(mce) : 0;

	int prev_sep = 1;
	int in_string = 0;
	int in_comment = (line->idx > 0 && E.line[line->idx - 1].hl_open_comment);

	int i = 0;
	while (i < line->rsize)
	{
		char c = line->render[i];
		unsigned char prev_hl = (i > 0) ? line->hl[i - 1] : HL_NORMAL;

		if (scs_len && !in_string && !in_comment)
		{
			if (!strncmp(&line->render[i], scs, scs_len))
			{
				memset(&line->hl[i], HL_COMMENT, line->rsize - i);
				break;
			}
		}

		if (mcs_len && mce_len && !in_string)
		{
			if (in_comment)
			{
				line->hl[i] = HL_MLCOMMENT;
				if (!strncmp(&line->render[i], mce, mce_len))
				{
					memset(&line->hl[i], HL_MLCOMMENT, mce_len);
					i += mce_len;
					in_comment = 0;
					prev_sep = 1;
					continue;
				}
				else
				{
					i++;
					continue;
				}
			}
			else if (!strncmp(&line->render[i], mcs, mcs_len))
			{
				memset(&line->hl[i], HL_MLCOMMENT, mcs_len);
				i += mcs_len;
				in_comment = 1;
				continue;
			}
		}

		if (E.syntax->flags & HL_HIGHLIGHT_STRINGS)
		{
			if (in_string)
			{
				line->hl[i] = HL_STRING;
				if (c == '\\' && i + 1 < line->rsize)
				{
					line->hl[i + 1] = HL_STRING;
					i += 2;
					continue;
				}
				if (c == in_string) in_string = 0;
				i++;
				prev_sep = 1;
				continue;
			}
			else
			{
				if (c == '"' || c == '\'')
				{
					in_string = c;
					line->hl[i] = HL_STRING;
					i++;
					continue;
				}
			}
		}

		if (E.syntax->flags & HL_HIGHLIGHT_NUMBERS)
		{
			if ((isdigit(c) && (prev_sep || prev_hl == HL_NUMBER)) || (c == '.' && prev_hl == HL_NUMBER))
			{
				line->hl[i] = HL_NUMBER;
				i++;
				prev_sep = 0;
				continue;
			}
		}

		if (prev_sep)
		{
			int j;
			for (j = 0; keywords[j]; j++)
			{
				int klen = strlen(keywords[j]);
				int kw2 = keywords[j][klen - 1] == '|';
				if (kw2) klen--;

				if (!strncmp(&line->render[i], keywords[j], klen) && is_separator(line->render[i + klen]))
				{
					memset(&line->hl[i], kw2 ? HL_KEYWORD2 : HL_KEYWORD1, klen);
					i += klen;
				}
			}

			if (keywords[j] != NULL)
			{
				prev_sep = 0;
				continue;
			}
		}

		prev_sep = is_separator(c);
		i++;
	}

	int changed = (line->hl_open_comment != in_comment);
	line->hl_open_comment = in_comment;
	if (changed && line->idx + 1 < E.linesnum)
		EditorUpdateSyntax(&E.line[line->idx + 1]);
}

int EditorSyntaxToColor(int hl)
{
	switch(hl)
	{
		case HL_COMMENT:
		case HL_MLCOMMENT: return 36;
		case HL_KEYWORD1: return 33;
		case HL_KEYWORD2: return 32;
		case HL_STRING: return 35;
		case HL_NUMBER: return 31;
		case HL_MATCH: return 34;
		default: return 37;
	}
}

void EditorSelectSyntaxHighlight()
{
	E.syntax = NULL;
	if (E.filename == NULL) return;

	char *ext = strrchr(E.filename, '.');

	for (unsigned int j = 0; j < HLDB_ENTRIES; j++)
	{
		struct EditorSyntax *s = &HLDB[j];
		unsigned int i = 0;
		while (s->filematch[i])
		{
			int is_ext = (s->filematch[i][0] == '.');
			if ((is_ext && ext && !strcmp(ext, s->filematch[i])) || (!is_ext && strstr(E.filename, s->filematch[i])))
			{
				E.syntax = s;

				int filerow;
				for (filerow = 0; filerow < E.linesnum; filerow++)
				{
					EditorUpdateSyntax(&E.line[filerow]);
				}
				return;
			}
			i++;
		}
	}
}

/*** Line Operations ***/
int EditorLineCxToRx(line_t *line, int cx)
{
	int j, rx = 0;
	for (j = 0; j < cx; j++)
	{
		if (line->bytes[j] == '\t')
			rx += (KILO_TAB_STOP - 1) - (rx % KILO_TAB_STOP);
		rx++;
	}

	return rx;
}

int EditorLineRxToCx(line_t *line, int rx)
{
	int cx, cur_rx = 0;
	for (cx = 0; cx < line->size; cx++)
	{
		if (line->bytes[cx] == '\t')
			cur_rx += (KILO_TAB_STOP - 1) - (cur_rx % KILO_TAB_STOP);
		cur_rx++;

		if (cur_rx > rx) return cx;
	}

	return cx;
}

void EditorUpdateLine(line_t *line)
{
	int j,
		idx = 0,
		tabs = 0;

	for (j = 0; j < line->size; j++)
		if (line->bytes[j] == '\t')
			tabs++;

	free(line->render);
	line->render = malloc(line->size + tabs * (KILO_TAB_STOP - 1) + 1);

	for (j = 0; j < line->size; j++)
	{
		if (line->bytes[j] == '\t')
		{
			line->render[idx++] = ' ';
			while (idx % KILO_TAB_STOP != 0)
				line->render[idx++] = ' ';
		}
		else
		{
			line->render[idx++] = line->bytes[j];
		}
	}
	line->render[idx] = '\0';
	line->rsize = idx;

	EditorUpdateSyntax(line);
}

void EditorInsertLine(int at, char *s, size_t len)
{
	if (at < 0 || at > E.linesnum) return;

	E.line = realloc(E.line, sizeof(line_t) * (E.linesnum + 1));
	memmove(&E.line[at + 1], &E.line[at], sizeof(line_t) * (E.linesnum - at));
	for (int j = at + 1; j <= E.linesnum; j++) E.line[j].idx++;

	E.line[at].idx = at;

	E.line[at].size = len;
	E.line[at].bytes = malloc(len + 1);
	memcpy(E.line[at].bytes, s, len);
	E.line[at].bytes[len] = '\0';
	E.line[at].render = NULL;
	E.line[at].rsize = 0;
	E.line[at].hl = NULL;
	E.line[at].hl_open_comment = 0;
	EditorUpdateLine(&E.line[at]);

	E.linesnum++;
	E.dirty++;
}

void EditorFreeLine(line_t *line)
{
	free(line->render);
	free(line->bytes);
	free(line->hl);
}

void EditorDelLine(int at)
{
	if (at < 0 || at >= E.linesnum) return;
	EditorFreeLine(&E.line[at]);
	memmove(&E.line[at], &E.line[at + 1], sizeof(line_t) * (E.linesnum - at - 1));
	for (int j = at; j <= E.linesnum - 1; j++) E.line[j].idx--;
	E.linesnum--;
	E.dirty++;
}

void EditorLineInsertChar(line_t *line, int at, int c)
{
	if (at < 0 || at > line->size)
		at = line->size;
	line->bytes = realloc(line->bytes, line->size + 2);
	memmove(&line->bytes[at + 1], &line->bytes[at], line->size - at + 1);
	line->size++;
	line->bytes[at] = c;
	EditorUpdateLine(line);
	E.dirty++;
}

void EditorLineAppendString(line_t *line, char *s, size_t len)
{
	line->bytes = realloc(line->bytes, line->size + len + 1);
	memcpy(&line->bytes[line->size], s, len);
	line->size += len;
	line->bytes[line->size] = '\0';
	EditorUpdateLine(line);
	E.dirty++;
}

void EditorLineDelChar(line_t *line, int at)
{
	if (at < 0 || at >= line->size) return;
	memmove(&line->bytes[at], &line->bytes[at + 1], line->size - at);
	line->size--;
	EditorUpdateLine(line);
	E.dirty++;
}

/*** Editor Operations ***/
void EditorInsertChar(int c)
{
	if (E.cursor.Y == E.linesnum)
	{
		EditorInsertLine(E.linesnum, "", 0);
	}
	EditorLineInsertChar(&E.line[E.cursor.Y], E.cursor.X, c);
	E.cursor.X++;
}

void EditorInsertNewLine(void)
{
	if (E.cursor.X == 0)
	{
		EditorInsertLine(E.cursor.Y, "", 0);
	}
	else
	{
		line_t *line = &E.line[E.cursor.Y];
		EditorInsertLine(E.cursor.Y + 1, &line->bytes[E.cursor.X], line->size - E.cursor.X);
		line = &E.line[E.cursor.Y];
		line->size = E.cursor.X;
		line->bytes[line->size] = '\0';
		EditorUpdateLine(line);
	}
	E.cursor.X = 0;
	E.cursor.Y++;
}

void EditorDelChar(void)
{
	if (E.cursor.Y == E.linesnum) return;
	if (E.cursor.X == 0 && E.cursor.Y == 0) return;

	line_t *line = &E.line[E.cursor.Y];
	if (E.cursor.X > 0)
	{
		EditorLineDelChar(line, E.cursor.X - 1);
		E.cursor.X--;
	}
	else
	{
		E.cursor.X = E.line[E.cursor.Y - 1].size;
		EditorLineAppendString(&E.line[E.cursor.Y - 1], line->bytes, line->size);
		EditorDelLine(E.cursor.Y);
		E.cursor.Y--;
	}
}

/*** File I/O ***/
char *EditorLinesToString(int *buflen)
{
	int j, totlen = 0;
	for (j = 0; j < E.linesnum; ++j)
		totlen += E.line[j].size + 1;
	*buflen = totlen;

	char *buf = malloc(totlen);
	char *p = buf;
	for (j = 0; j < E.linesnum; ++j)
	{
		memcpy(p, E.line[j].bytes, E.line[j].size);
		p += E.line[j].size;
		*p = '\n';
		p++;
	}
	p--;
	*p = '\0';

	return buf;
}

void EditorOpen(char *filename)
{
	free(E.filename);
	E.filename = strdup(filename);

	EditorSelectSyntaxHighlight();

	FILE *fp = fopen(filename, "r");
	if (!fp)
	{
		perror("File Open: ");
		getchar();
		exit(1);
	}

	char buf[64];
	char *line = NULL;
	size_t nread;
	size_t linelen = 0;
	size_t linecap = 0;
	bool newline = false;

	while (fgets(buf, sizeof(buf), fp) != NULL)
	{
		nread = strlen(buf);
		while (nread > 0 && (buf[nread - 1] == '\n' || buf[nread - 1] == '\r'))
		{
			nread--;
			newline = true;
		}

		if (linecap < linelen + nread)
		{
			line = realloc(line, linelen + nread);
			linecap = linelen + nread;
		}

		memcpy(&line[linelen], buf, nread);
		linelen += nread;

		if (newline)
		{
			EditorInsertLine(E.linesnum, line, linelen);
			newline = false;
			linelen = 0;
		}
	}
	
	EditorInsertLine(E.linesnum, line, linelen);
	free(line);
	fclose(fp);
	E.dirty = 0;
}

void EditorSave(void)
{
	if (E.filename == NULL)
	{
		E.filename = EditorPrompt("Save as: %s (ESC to cancel)", NULL);
		if (E.filename == NULL)
		{
			EditorSetStatusMessage("Save aborted");
			return;
		}
		EditorSelectSyntaxHighlight();
	}

	int len;
	char *buf = EditorLinesToString(&len);

	FILE *fp = fopen(E.filename, "w+");
	if (fp != NULL)
	{
		if (fwrite(buf, 1, len, fp) == len)
		{
			fclose(fp);
			free(buf);
			E.dirty = 0;
			EditorSetStatusMessage("%d bytes written to disk", len);
			return;
		}
		fclose(fp);
	}
	free(buf);
	EditorSetStatusMessage("Can't save! I/O error: %s", strerror(errno));	
}

/*** Find ***/
void EditorFindCallback(char *query, int key)
{
	static int last_match = -1;
	static int direction = 1;

	static int saved_hl_line;
	static char *saved_hl = NULL;

	if (saved_hl)
	{
		memcpy(E.line[saved_hl_line].hl, saved_hl, E.line[saved_hl_line].rsize);
		free(saved_hl);
		saved_hl = NULL;
	}

	if (key == '\r' || key == '\x1b')
	{
		last_match = -1;
		direction = 1;
		return;
	}
	else if (key == ARROW_RIGHT || key == ARROW_DOWN)
	{
		direction = 1;
	}
	else if (key == ARROW_LEFT || key == ARROW_UP)
	{
		direction = -1;
	}
	else
	{
		last_match = -1;
		direction = 1;
	}

	if (last_match == -1)
		direction = 1;

	int current = last_match;
	int i;
	for (i = 0; i < E.linesnum; ++i)
	{
		current += direction;
		if (current == -1)
			current = E.linesnum - 1;
		else if (current == E.linesnum)
			current = 0;

		line_t *line = &E.line[current];
		char *match = strstr(line->render, query);
		if (match)
		{
			last_match = current;
			E.cursor.Y = current;
			E.offset.Y = E.linesnum;
			E.cursor.X = EditorLineRxToCx(line, match - line->render);
			saved_hl_line = current;
			saved_hl = malloc(line->rsize);
			memcpy(saved_hl, line->hl, line->rsize);
			memset(&line->hl[match - line->render], HL_MATCH, strlen(query));
			break;
		}
	}
}

void EditorFind(void)
{
	COORD saved_cursor = E.cursor;
	COORD saved_offset = E.offset;

	char *query = EditorPrompt("Search: %s (use Arrows, ESC or Enter)", EditorFindCallback);
	
	if (query)
	{
		free(query);
	}
	else
	{
		E.cursor = saved_cursor;
		E.offset = saved_offset;
	}
}

/*** Append Buffer ***/
struct abuf {
	char *b;
	int len;
};

#define ABUF_INIT {NULL, 0}

void abAppend(struct abuf *ab, const char *s, int len)
{
	char *new = realloc(ab->b, ab->len + len);

	if (new == NULL) return;
	memcpy(&new[ab->len], s, len);
	ab->b = new;
	ab->len += len;
}

void abFree(struct abuf *ab)
{
	free(ab->b);
}

/*** Output ***/
void EditorScroll(void)
{
	E.rx = E.cursor.X;
	if (E.cursor.Y < E.linesnum)
		E.rx = EditorLineCxToRx(&E.line[E.cursor.Y], E.cursor.X);

	if (E.cursor.Y < E.offset.Y)
		E.offset.Y = E.cursor.Y;
	if (E.cursor.Y >= E.offset.Y + E.bufSize.Y)
		E.offset.Y = E.cursor.Y - E.bufSize.Y + 1;
	if (E.rx < E.offset.X)
		E.offset.X = E.rx;
	if (E.rx >= E.offset.X + E.bufSize.X)
		E.offset.X = E.rx - E.bufSize.X + 1;
}

void EditorDrawLines(struct abuf *ab)
{
	int i;
	int filerow;
	for (i = 0; i < E.bufSize.Y; ++i)
	{
		filerow = i + E.offset.Y;
		if (filerow >= E.linesnum)
		{
			if (E.linesnum == 0 && i == E.bufSize.Y / 3)
			{
				char welcome[80];
				int welcomelen = snprintf(
					welcome, 
					sizeof(welcome), 
					"WinKilo Editor -- Version %s", 
					KILO_VERSION
				);
				if (welcomelen > E.bufSize.X)
					welcomelen = E.bufSize.X;
				int padding = (E.bufSize.X - welcomelen) / 2;
				if (padding)
				{
					abAppend(ab, "~", 1);
					padding--;
				}
				while (padding--)
					abAppend(ab, " ", 1);
				abAppend(ab, welcome, welcomelen);
			}
			else
				abAppend(ab, "~", 1);
		}
		else
		{
			int len = E.line[filerow].rsize - E.offset.X;
			if (len < 0) len = 0;
			if (len > E.bufSize.X) len = E.bufSize.X;
			char *c = &E.line[filerow].render[E.offset.X];
			unsigned char *hl = &E.line[filerow].hl[E.offset.X];
			int current_color = -1;
			int j;
			for (j = 0; j < len; j++)
			{
				if (iscntrl(c[j]))
				{
					char sym = (c[j] <= 26) ? '@' + c[j] : '?';
					abAppend(ab, "\x1b[7m", 4);
					abAppend(ab, &sym, 1);
					abAppend(ab, "\x1b[m", 3);
					if (current_color != -1)
					{
						char buf[16];
						int clen = snprintf(buf, sizeof(buf), "\x1b[%dm", current_color);
						abAppend(ab, buf, clen);
					}
				}
				else if (hl[j] == HL_NORMAL)
				{
					if (current_color != -1)
					{
						abAppend(ab, "\x1b[39m", 5);
						current_color = -1;
					}
					abAppend(ab, &c[j], 1);
				}
				else
				{
					int color = EditorSyntaxToColor(hl[j]);
					if (color != current_color)
					{
						current_color = color;
						char buf[16];
						int clen = snprintf(buf, sizeof(buf), "\x1b[%dm", color);
						abAppend(ab, buf, clen);
					}
					abAppend(ab, &c[j], 1);
				}
			}
			abAppend(ab, "\x1b[39m", 5);
		}
		abAppend(ab, "\x1b[K", 3);
		abAppend(ab, "\r\n", 2);
	}
}

void EditorDrawStatusBar(struct abuf *ab)
{
	int len, rlen;
	char status[80], rstatus[80];

	abAppend(ab, "\x1b[7m", 4);
	len = snprintf(
		status, 
		sizeof(status), 
		"%.20s - %d lines %s",
		E.filename ? E.filename : "[UNTITLED]",
		E.linesnum,
		E.dirty ? "(modified)" : ""
	);
	rlen = snprintf(
		rstatus,
		sizeof(rstatus),
		"%s - %d/%d",
		E.syntax ? E.syntax->filetype : "no ft",
		E.cursor.Y + 1,
		E.linesnum
	);
	if (len > E.bufSize.X) 
		len = E.bufSize.X;
	abAppend(ab, status, len);
	while (len < E.bufSize.X)
	{
		if (E.bufSize.X - len == rlen)
		{
			abAppend(ab, rstatus, rlen);
			break;
		}
		else
		{
			abAppend(ab, " ", 1);
			len++;
		}
	}
	abAppend(ab, "\x1b[m", 3);
	abAppend(ab, "\r\n", 2);
}

void EditorDrawMessageBar(struct abuf *ab)
{
	int msglen;
	abAppend(ab, "\x1b[K", 3);
	msglen = strlen(E.statusmsg);
	if (msglen > E.bufSize.X)
		msglen = E.bufSize.X;
	if (msglen && time(NULL) - E.statusmsg_time < 5)
		abAppend(ab, E.statusmsg, msglen);
}

void EditorRefreshScreen(void)
{
	char buf[32];
	struct abuf ab = ABUF_INIT;
	EditorScroll();
	abAppend(&ab, "\x1b[?25l", 6);
	abAppend(&ab, "\x1b[H", 3);
	EditorDrawLines(&ab);
	EditorDrawStatusBar(&ab);
	EditorDrawMessageBar(&ab);
	snprintf(buf, 
		sizeof(buf), 
		"\x1b[%d;%dH", 
		(E.cursor.Y - E.offset.Y) + 1,
		(E.rx - E.offset.X) + 1
	);
	abAppend(&ab, buf, strlen(buf));
	abAppend(&ab, "\x1b[?25h", 6);
	fwrite(ab.b, 1, ab.len, stdout);
	abFree(&ab);
}

void EditorSetStatusMessage(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(E.statusmsg, sizeof(E.statusmsg), fmt, ap);
	va_end(ap);
	E.statusmsg_time = time(NULL);
}

/*** Input ***/
char *EditorPrompt(char *prompt, void (*callback)(char *, int))
{
	int c;
	size_t bufsize = 128;
	char *buf = malloc(bufsize);

	size_t buflen = 0;
	buf[0] = '\0';

	while (1)
	{
		EditorSetStatusMessage(prompt, buf);
		EditorRefreshScreen();

		c = HandleInputs();

		if (!c) continue;

		if (c == DEL_KEY || c == CTRL_KEY('h') || c == BACKSPACE)
		{
			if (buflen != 0)
				buf[--buflen] = '\0';
		}
		if (c == '\x1b')
		{
			EditorSetStatusMessage("");
			if (callback)
				callback(buf, c);
			free(buf);
			return NULL;
		}
		else if (c == '\r')
		{
			if (buflen != 0)
			{
				EditorSetStatusMessage("");
				if (callback)
					callback(buf, c);
				return buf;
			}
		}
		else if (!iscntrl(c) && c < 128)
		{
			if (buflen == bufsize - 1)
			{
				bufsize *= 2;
				buf = realloc(buf, bufsize);
			}
			buf[buflen++] = c;
			buf[buflen] = '\0';
		}

		if (callback)
			callback(buf, c);
	}
}

void EditorMoveCursor(int key)
{
	line_t *line = (E.cursor.Y >= E.linesnum) ? NULL : &E.line[E.cursor.Y];

	switch (key)
	{
		case ARROW_LEFT:
			if (E.cursor.X != 0)
				E.cursor.X--;
			else if (E.cursor.Y > 0)
			{
				E.cursor.Y--;
				E.cursor.X = E.line[E.cursor.Y].size;
			}
			break;
		case ARROW_RIGHT:
			if (line && E.cursor.X < line->size)
				E.cursor.X++;
			else if (line && E.cursor.X == line->size)
			{
				E.cursor.X = 0;
				E.cursor.Y++;
			}
			break;
		case ARROW_UP:
			if (E.cursor.Y != 0)
				E.cursor.Y--;
			break;
		case ARROW_DOWN:
			if (E.cursor.Y < E.linesnum)
				E.cursor.Y++;
			break;
	}

	line = (E.cursor.Y >= E.linesnum) ? NULL : &E.line[E.cursor.Y];
	int linelen = line ? line->size : 0;
	if (E.cursor.X > linelen) E.cursor.X = linelen;
}

void HandleKeyPress(void)
{
	static int quit_times = KILO_QUIT_TIMES;

	int c = HandleInputs();

	switch (c)
	{
		case 0: 
			// Ignore Shift and Ctrl keys
			// Requied to prevent reseting quit_times
			return;
			break;
		case '\r':
			EditorInsertNewLine();
			break;
		case CTRL_KEY('q'):
			if (E.dirty && quit_times > 0)
			{
				EditorSetStatusMessage(
					"WARNING: File has unsaved changes. " 
					"Press Ctrl-Q %d more times to quit.",
					quit_times
				);
				quit_times--;
				return;
			}
			exit(0);
			break;
		case CTRL_KEY('s'):
			EditorSave();
			break;
		case HOME_KEY:
			E.cursor.X = 0;
			break;
		case END_KEY:
			if (E.cursor.Y < E.linesnum)
				E.cursor.X = E.line[E.cursor.Y].size;
			break;
		case CTRL_KEY('f'):
			EditorFind();
			break;
		case BACKSPACE:
		case CTRL_KEY('h'):
		case DEL_KEY:
			if (c == DEL_KEY)
				EditorMoveCursor(ARROW_RIGHT);
			EditorDelChar();
			break;
		case PAGE_UP:
		case PAGE_DOWN:
			{
				if (c == PAGE_UP)
				{
					E.cursor.Y = E.offset.Y;
				}
				else if (c == PAGE_DOWN)
				{
					E.cursor.Y = E.offset.Y + E.bufSize.Y - 1;
					if (E.cursor.Y > E.linesnum)
						E.cursor.Y = E.linesnum;
				}
				int times = E.bufSize.Y;
				while (times--)
					EditorMoveCursor(c == PAGE_UP ? ARROW_UP : ARROW_DOWN);
			}
			break;
		case ARROW_UP:
		case ARROW_DOWN:
		case ARROW_LEFT:
		case ARROW_RIGHT:
			EditorMoveCursor(c);
			break;
		case CTRL_KEY('l'):
		case '\x1b':
			break;
		default:
			EditorInsertChar(c);
			break;
	}

	quit_times = KILO_QUIT_TIMES;
}

// https://learn.microsoft.com/en-us/windows/console/reading-input-buffer-events
int HandleInputs(void)
{
	int i;
	int c;
	DWORD cInRead;
	INPUT_RECORD irInBuf[MAXINREC];

	if (!ReadConsoleInput(E.hStdin, irInBuf, 128, &cInRead))
	{
		fprintf(stderr, "Error read input events: (%d)\n", GetLastError());
		exit(1);
	}

	for (i = 0; i < cInRead; ++i)
	{
		switch (irInBuf[i].EventType)
		{
			case KEY_EVENT:
				if (!irInBuf[i].Event.KeyEvent.bKeyDown)
					continue;

				c = irInBuf[i].Event.KeyEvent.uChar.AsciiChar;
				
				if (c == '\x1b' && (i + cInRead) > 2)
				{
					i++;
					char seq[3];
					seq[0] = irInBuf[i].Event.KeyEvent.uChar.AsciiChar; i++;
					seq[1] = irInBuf[i].Event.KeyEvent.uChar.AsciiChar; i++;

					if (seq[0] == '[')
					{
						if (seq[1] >= '0' && seq[1] <= '9')
						{
							seq[2] = irInBuf[i].Event.KeyEvent.uChar.AsciiChar; i++;

							if (seq[2] == '~')
							{
								switch (seq[1])
								{
									case '1': c = HOME_KEY; break;
									case '3': c = DEL_KEY; break;
									case '4': c = END_KEY; break;
									case '5': c = PAGE_UP; break;
									case '6': c = PAGE_DOWN; break;
									case '7': c = HOME_KEY; break;
									case '8': c = END_KEY; break;
								}
							}
						}
						else
						{
							switch (seq[1])
							{
								case 'A': c = ARROW_UP; break;
								case 'B': c = ARROW_DOWN; break;
								case 'C': c = ARROW_RIGHT; break;
								case 'D': c = ARROW_LEFT; break;
								case 'H': c = HOME_KEY; break;
								case 'F': c = END_KEY; break;
							}
						}
					}
					else if (seq[0] == 'O')
					{
						switch (seq[1])
						{
							case 'H': c = HOME_KEY; break;
							case 'F': c = END_KEY; break;
						}
					}
				}

				return c;
				break;

			case WINDOW_BUFFER_SIZE_EVENT:
				E.bufSize.X = irInBuf[i].Event.WindowBufferSizeEvent.dwSize.X; 
				E.bufSize.Y = irInBuf[i].Event.WindowBufferSizeEvent.dwSize.Y - 2;
				break;
		}
	}

	return 0;
}

/*** Initialize ***/
int InitEditorConsole(void)
{
	DWORD dwMode;
	CONSOLE_SCREEN_BUFFER_INFO csbi;

	E.hStdout = GetStdHandle(STD_OUTPUT_HANDLE);
	E.hStdin = GetStdHandle(STD_INPUT_HANDLE);

	if (E.hStdout == INVALID_HANDLE_VALUE)
	{
		fprintf(stderr, "Error: Invalid stdout handle value.\n");
		return 0;
	}

	if (E.hStdin == INVALID_HANDLE_VALUE)
	{
		fprintf(stderr, "Error: Invalid stdin handle value.\n");
		return 0;
	}
    	
	// Get console mode.
    	if (!GetConsoleMode(E.hStdout, &E.dwOutMode))
	{
		fprintf(stderr, "Error: Can't get stdout handle mode (%d).\n", GetLastError());
		return 0;
	}

	if (!GetConsoleMode(E.hStdin, &E.dwInMode))
	{
		fprintf(stderr, "Error: Can't get stdin handle mode (%d).\n", GetLastError());
		return 0;
	}

	// Enable virtual terminal sequences.
	dwMode = E.dwOutMode;
	dwMode |= ENABLE_PROCESSED_OUTPUT; 
	dwMode |= ENABLE_VIRTUAL_TERMINAL_PROCESSING;

    	if (!SetConsoleMode(E.hStdout, dwMode))
	{
		fprintf(stderr, "Error: Can't set stdout handle mode (%d).\n", GetLastError());
		return 0;
	}

	// Enable mouse and key input events.
	E.dwInMode &= (~ENABLE_VIRTUAL_TERMINAL_INPUT);
	E.dwInMode |= ENABLE_ECHO_INPUT;
	E.dwInMode |= ENABLE_LINE_INPUT;
	E.dwInMode |= ENABLE_PROCESSED_INPUT;
	E.dwInMode |= ENABLE_QUICK_EDIT_MODE;

	dwMode = E.dwInMode;

	dwMode &= (~ENABLE_ECHO_INPUT);		// Required to prevent characters from showing
	dwMode &= (~ENABLE_LINE_INPUT);		// Required to process every single character without pressing 'Enter'
	dwMode &= (~ENABLE_PROCESSED_INPUT);	// Disable Ctrl-C
	dwMode |= ENABLE_EXTENDED_FLAGS; 
	dwMode |= ENABLE_WINDOW_INPUT; 
	dwMode |= ENABLE_VIRTUAL_TERMINAL_INPUT;

	if (!SetConsoleMode(E.hStdin, dwMode))
	{
		fprintf(stderr, "Error: Can't set stdin handle mode (%d).\n", GetLastError());
		return 0;
	}

	// Switch to a new alternate screen buffer.
	printf("\x1b[?1049h");
	printf("\x1b]0;%s\x07", KILO_TITLE);

	// Retrieves information about the current console screen buffer.
	GetConsoleScreenBufferInfo(E.hStdout, &csbi);
	E.bufSize.Y = csbi.dwSize.Y - 2;
	E.bufSize.X = csbi.dwSize.X; 
	E.offset.X = 0;
	E.offset.Y = 0;
	E.cursor.X = 0;
	E.cursor.Y = 0;
	E.rx = 0;
	E.linesnum = 0;
	E.line = NULL;
	E.dirty = 0;
	E.filename = NULL;
	E.statusmsg[0] = '\0';
	E.statusmsg_time = 0;
	E.syntax = NULL;

	return 1;
}

void ExitEditorConsole(void)
{
	free(E.filename);
	free(E.line);

	// Reset console settings.
	if (E.hStdout != INVALID_HANDLE_VALUE && E.dwOutMode)
	{
		printf("\x1b[!p");		// Reset console settings.
		printf("\x1b[?1049l");	// Switch back to the main buffer.
		SetConsoleMode(E.hStdout, E.dwOutMode);
	}

	if (E.hStdin != INVALID_HANDLE_VALUE && E.dwInMode)
	{
		SetConsoleMode(E.hStdin, E.dwInMode);
	}
}

int main(int argc, char *argv[])
{
	atexit(ExitEditorConsole);
	
	if (!InitEditorConsole()) exit(1);

	if (argc > 1)
	{
		EditorOpen(argv[1]);
	}

	EditorSetStatusMessage("HELP: Ctrl-F = find | Ctrl-S = save | Ctrl-Q = quit");
	
	while (1)
	{
		EditorRefreshScreen();
		HandleKeyPress();
	}
	
	return 0;
}
