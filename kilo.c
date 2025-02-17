/*** includes ***/

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <termios.h>

/*** defines ***/
#define KILO_VERSION "0.0.1"

#define CTRL_KEY(k) ((k) & 0x1f)

enum editorKey {
  ARROW_LEFT = 'a',
  ARROW_RIGHT = 'd',
  ARROW_UP = 'w',
  ARROW_DOWN = 's'
};
/*** data ***/

struct editorConfig {
  int cx, cy;
  int screenrows;
  int screencols;
  struct termios orig_termios;
};

struct editorConfig E;

/*** terminal ***/

/* perror() looks at the global errno variable 
 * and prints a descriptive error message for it.
 */
void die(const char *s) {
  write(STDIN_FILENO, "\x1b[2J", 4);
  write(STDIN_FILENO, "\x1b[H", 3);

  perror(s);
  exit(1);
}

void disableRawMode(void){
  if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &E.orig_termios) == -1) 
    die("tcsetattr");
}

void enableRawMode(void) {
  if (tcgetattr(STDIN_FILENO, &E.orig_termios) == -1)
    die("tcgetattr");
  atexit(disableRawMode);

  struct termios raw = E.orig_termios;
  raw.c_iflag &= ~(BRKINT | ICRNL | INPCK| ISTRIP | IXON);
  raw.c_oflag &= ~(OPOST);
  raw.c_cflag &= (CS8);
  raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);

  /* 
   * VMIN sets the minimum number of bytes of input needed before 
   * read() can return. We set it to 0 so that read() returns as soon
   * as there is any input to be read.
   */
  raw.c_cc[VMIN] = 0;

  /*
   * VTIME sets max amount of time to wait before read() returns.
   * It is in tenths of a second, so we set it to 1/10 of a sec 
   * or 100 milliseconds.
   */
  raw.c_cc[VTIME] = 1;

  if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1) die("tcsetattr");
}

char editorReadKey(void) {
  int nread;
  char c;
  while ((nread = read(STDIN_FILENO, &c, 1)) != 1) {
    if (nread == -1 && errno != EAGAIN) die("read");
  }

  if (c == '\x1b') {
    char seq[3];

    if (read(STDIN_FILENO, &seq[0], 1) != 1) return '\x1b';
    if (read(STDIN_FILENO, &seq[1], 1) != 1) return '\x1b';

    if (seq[0] == '[') {
      switch(seq[1]) {
        case 'A' : return ARROW_UP;
        case 'B' : return ARROW_DOWN;
        case 'C' : return ARROW_RIGHT;
        case 'D' : return ARROW_LEFT;
      }
    }

    return '\x1b';
  } else {
    return c;
  }
}

int getCursorPosition(int *rows, int *cols) {
  char buf[32];
  unsigned int i = 0;

  if (write(STDOUT_FILENO, "x\1b[6n", 4) != 4) return -1;

  while( i < sizeof(buf) - 1) {
    if (read(STDOUT_FILENO, &buf[i], 1) != 1)  break;
    if (buf[i] == 'R') break;
    i++;
  }
  buf[i] = '\0';

  if (buf[0] != '\x1b' || buf[1] != '[') return -1;
  if (sscanf(&buf[2], "%d;%d", rows, cols) != 2) return -1;

  return 0;
}

int getWindowSize(int *rows, int *cols) {
  struct winsize ws;
  if (ioctl(STDOUT_FILENO, TIOCGSIZE, &ws) == -1 || ws.ws_col == 0) {
    if (write(STDOUT_FILENO, "x\1b[999c\x1b[999B", 12) != 12) return -1;
    return getCursorPosition(rows, cols);
    return -1;
  } else {
    *cols = ws.ws_col;
    *rows = ws.ws_row;
    return 0;
  }
}

/*** append buffer ***/

struct abuf{
  char *b;
  int len;
};

#define ABUF_INIT {NULL, 0}

void abAppend(struct abuf *ab, const char *s, int len) {
  char *new = realloc(ab->b, ab->len + len);

  if (new == NULL) return;
  memcpy(&new[ab->len], s, len);
  ab->b = new;
  ab->len += len;
}

void abFree(struct abuf *ab) {
  free(ab->b);
}

/*** output ***/

void editorDrawRows(struct abuf *ab) {
  int y;
  for (y = 0; y < E.screenrows; y++) {
    if (y == E.screenrows / 3) {
      char welcome[80];
      int welcomelen = snprintf(welcome, sizeof(welcome),
                                "Kilo editor -- version %s", KILO_VERSION);

      if (welcomelen >  E.screencols) welcomelen = E.screencols;
      int padding = (E.screencols - welcomelen) / 2;
      if (padding) {
        abAppend(ab, "~", 1);
        padding--;
      }

      while (padding--) abAppend(ab, " ", 1);
      abAppend(ab, welcome, welcomelen);
    } else {
      abAppend(ab, "~", 1);
    }

    abAppend(ab, "\x1b[K", 3);
    if (y < E.screenrows - 1) {
      abAppend(ab, "\r\n", 2);
    }
  }
}

void editorRefreshScreen(void) {
  struct abuf ab = ABUF_INIT;

  abAppend(&ab, "\x1b[?25l", 6);
  abAppend(&ab, "\x1b[H", 3);

  editorDrawRows(&ab);
  char buf[32];
  snprintf(buf, sizeof(buf), "\x1b[%d;%dH", E.cy + 1, E.cx + 1);
  abAppend(&ab, buf, strlen(buf));

  abAppend(&ab, "\x1b[?25h", 6);

  write(STDOUT_FILENO, ab.b, ab.len);
  abFree(&ab);
}

/*** input ***/

void editorMoveCursor(char key) {
  switch(key) {
    case ARROW_LEFT:
      E.cx--;
      break;
    case ARROW_RIGHT:
      E.cx++;
      break;
    case ARROW_UP:
      E.cy--;
      break;
    case ARROW_DOWN:
      E.cy++;
      break;
  }
}

void editoreProcessKeypress(void) {
  char c = editorReadKey();

  switch (c) {
    case CTRL_KEY('q'):
      write(STDIN_FILENO, "\x1b[2J", 4);
      write(STDIN_FILENO, "\x1b[H", 3);

      exit(0);
      break;

    case ARROW_UP:
    case ARROW_DOWN:
    case ARROW_LEFT:
    case ARROW_RIGHT:
      editorMoveCursor(c);
      break;
  }
}

/*** init ***/

void initEditor(void) {
  E.cx = 0;
  E.cy = 0;

  if (getWindowSize(&E.screenrows, &E.screencols) == -1 ) die("getWindowSize");
}
int main(void) {
  enableRawMode();
  initEditor();

  while(1) {
    editorRefreshScreen();
    editoreProcessKeypress();
  }
  return 0;
}
