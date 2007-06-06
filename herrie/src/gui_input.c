/*
 * Copyright (c) 2006-2007 Ed Schouten <ed@fxq.nl>
 * All rights reserved.
 * 
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */
/**
 * @file gui_input.c
 * @brief Keyboard and signal input for user interface.
 */

#include "stdinc.h"

#include "audio_output.h"
#include "config.h"
#include "gui_internal.h"
#include "playq.h"
#include "scrobbler.h"

/**
 * @brief The focus is on the browser.
 */
#define GUI_FOCUS_BROWSER	0
/**
 * @brief The focus is on the playlist.
 */
#define GUI_FOCUS_PLAYQ		1
/**
 * @brief The number of focusable windows.
 */
#define GUI_FOCUS_COUNT		2
/**
 * @brief Window that is currently focused.
 */
static int gui_input_curfocus = GUI_FOCUS_BROWSER;
/**
 * @brief Indicator of the current search string.
 */
static char *cursearch = NULL;
/**
 * @brief The last seek string that has been entered.
 */
static char *curseek = NULL;

/**
 * @brief Fetch a character from the keyboard, already processing
 *        terminal resizes.
 */
static int
gui_input_getch(void)
{
	int ch;

	do {
		ch = getch();
		/* Redraw everything when we get KEY_RESIZE or ^L */
		if (ch == KEY_RESIZE || ch == 0x0c) {
			gui_draw_resize();
			ch = ERR;
		}
	} while (ch == ERR);

	/* End-key on xBSD is select? */
	if (ch == KEY_SELECT)
		return (KEY_END);

	/* ^H or ^? backspace */
	if (ch == 0x08 || ch == 0x7f)
		return (KEY_BACKSPACE);
	
	/* Valid character */
	return (ch);
}

/**
 * @brief Switch the focus to the next window.
 */
static void
gui_input_switchfocus(void)
{
	gui_input_curfocus++;
	gui_input_curfocus %= GUI_FOCUS_COUNT;

	/* Update the selection colors */
	gui_playq_setfocus(gui_input_curfocus == GUI_FOCUS_PLAYQ);
	gui_browser_setfocus(gui_input_curfocus == GUI_FOCUS_BROWSER);
}

/**
 * @brief Ask the user to enter a search string when none was given and
 *        search for the next item matching the search string.
 */
static void
gui_input_searchnext(void)
{
	int nfocus = GUI_FOCUS_PLAYQ;

	if (cursearch == NULL) {
		/* No search string yet */
		cursearch = gui_input_askstring(_("Search for"), NULL, NULL);
		if (cursearch == NULL)
			return;
	}

	/*
	 * We want to change our search order depending on which dialog
	 * is currently focused. This code is quite awful, but does the
	 * thing. When the playq is focused, it only performs the first
	 * two searches. If the browser is focused, it only performs the
	 * last two.
	 */
	if (gui_input_curfocus == GUI_FOCUS_PLAYQ &&
	    gui_playq_searchnext(cursearch) == 0) {
		goto found;
	} else if (gui_browser_searchnext(cursearch) == 0) {
		nfocus = GUI_FOCUS_BROWSER;
		goto found;
	} else if (gui_input_curfocus != GUI_FOCUS_PLAYQ &&
	    gui_playq_searchnext(cursearch) == 0) {
		goto found;
	}

	/* Bad luck. */
	gui_msgbar_warn(_("Not found."));
	return;

found:	/* Focus the window with the match and redraw them. */
	gui_input_curfocus = nfocus;
	gui_playq_setfocus(gui_input_curfocus == GUI_FOCUS_PLAYQ);
	gui_browser_setfocus(gui_input_curfocus == GUI_FOCUS_BROWSER);
}

/**
 * @brief Ask the user to enter a new search string and perform the
 *        first search.
 */
static void
gui_input_search(void)
{
	char *str;

	/* Allow the user to enter a search string */
	str = gui_input_askstring(_("Search for"), cursearch, NULL);
	if (str == NULL)
		return;

	/* Replace our search string */
	g_free(cursearch);
	cursearch = str;

	gui_input_searchnext();
}

/**
 * @brief Properly shutdown the application by stopping playback and
 *        destroying the GUI.
 */
static void
gui_input_quit(void)
{
	playq_shutdown();
#ifdef BUILD_SCROBBLER
	scrobbler_shutdown();
#endif /* BUILD_SCROBBLER */
	audio_output_close();
	gui_draw_destroy();
	exit(0);
}

/**
 * @brief Prompt the user with a message to confirm termination of the
 *        application.
 */
static void
gui_input_askquit(void)
{
	int ret;
	char *msg;

	if (!config_getopt_bool("gui.input.may_quit")) {
		gui_msgbar_warn(_("Use kill(1) to quit."));
		return;
	}

	msg = g_strdup_printf(_("Quit %s?"), APP_NAME);
	ret = gui_input_askyesno(msg);
	g_free(msg);

	if (ret == 0)
		gui_input_quit();
}

/**
 * @brief Instruct the playlist to seek the current song 5 seconds
 *        backward.
 */
static void
gui_input_cursong_seek_backward(void)
{
	playq_cursong_seek(-5, 1);
}

/**
 * @brief Instruct the playlist to seek the current song 5 seconds
 *        forward.
 */
static void
gui_input_cursong_seek_forward(void)
{
	playq_cursong_seek(5, 1);
}

/**
 * @brief Ask the user to specify a position to seek the current song to.
 */
static void
gui_input_cursong_seek_jump(void)
{
	char *str, *t;
	int total = 0, split = 0, digit = 0, value, relative = 0;

	t = str = gui_input_askstring(_("Jump to position"),
	    curseek, "1234567890:+-");
	if (str == NULL)
		return;

	for (t = str; *t != '\0'; t++) {
		switch (*t) {
		case ':':
			/*
			 * Only allow two :'s, not without a prepending
			 * digit. :'s must be interleaved with two
			 * digits.
			 */
			if (split > 1 || digit == 0 ||
			    (split > 0 && digit != 2))
				goto bad;
			split++;
			digit = 0;
			break;
		case '+':
			/* Must be at the beginning */
			if (t != str)
				goto bad;
			relative = 1;
			break;
		case '-':
			/* Must be at the beginning */
			if (t != str)
				goto bad;
			relative = -1;
			break;
		default:
			/* Regular digit */
			value = g_ascii_digit_value(*t);
			g_assert(value != -1);
			/* Only allow 0-5 to be used for the first digit. */
			if (split > 0 && digit == 0 && value > 5)
				goto bad;
			total *= (digit == 0) ? 6 : 10;
			total += value;
			digit++;
		}
	}

	/* Too many trailing digits */
	if (split > 0 && digit != 2)
		goto bad;

	if (relative != 0)
		total *= relative;
	playq_cursong_seek(total, relative);

	/* Show the string the next time as well */
	g_free(curseek);
	curseek = str;
	return;

bad:	gui_msgbar_warn(_("Bad time format."));
	g_free(str);
}

/**
 * @brief A simple binding from a keyboard character input to a function.
 */
struct gui_binding {
	/**
	 * @brief The window that should be focussed.
	 */
	int focus;
	/**
	 * @brief The character that should be pressed. 
	 */
	int input;
	/**
	 * @brief The function that will be run.
	 */
	void (*func)(void);
};

/**
 * @brief List of keybindings available in the GUI.
 */
static struct gui_binding kbdbindings[] = {
	/* Application-wide keyboard bindings */
	{ -1, '<',			gui_input_cursong_seek_backward, },
	{ -1, '>',			gui_input_cursong_seek_forward, },
	{ -1, 'a',			gui_browser_playq_add_after },
	{ -1, 'A',			gui_browser_playq_add_tail },
	{ -1, 'b',			playq_cursong_next },
	{ -1, 'c',			playq_cursong_pause, },
	{ -1, 'C',			gui_browser_chdir },
	{ -1, 'd',			gui_playq_song_remove },
	{ -1, 'D',			gui_playq_song_remove_all },
	{ -1, 'h',			gui_browser_dir_parent },
	{ -1, 'i',			gui_browser_playq_add_before },
	{ -1, 'I',			gui_browser_playq_add_head },
	{ -1, 'J',			gui_input_cursong_seek_jump, }, /* ^J */
	{ -1, 'l',			gui_browser_dir_enter },
	{ -1, 'q',			gui_input_askquit },
	{ -1, 'r',			playq_repeat_toggle },
	{ -1, 'R',			gui_playq_song_randomize },
	{ -1, 'v',			playq_cursong_stop, },
	{ -1, 'w',			gui_browser_write_playlist },
	{ -1, 'x',			gui_playq_song_select },
	{ -1, 'z',			playq_cursong_prev },
	{ -1, '[',			gui_playq_song_moveup },
	{ -1, ']',			gui_playq_song_movedown },
	{ -1, '\t', 			gui_input_switchfocus },
	{ -1, 0x17, 			gui_input_switchfocus }, /* ^W */
	{ -1, '/',			gui_input_search },
	{ -1, 'n',			gui_input_searchnext },
	{ -1, KEY_LEFT,			gui_browser_dir_parent },
	{ -1, KEY_RIGHT,		gui_browser_dir_enter },

	/* Keyboard bindings for the file browser */
	{ GUI_FOCUS_BROWSER, ' ',	gui_browser_cursor_pagedown },
	{ GUI_FOCUS_BROWSER, 'f',	gui_browser_fullpath },
	{ GUI_FOCUS_BROWSER, 'G',	gui_browser_cursor_bottom },
	{ GUI_FOCUS_BROWSER, 'g',	gui_browser_cursor_top },
	{ GUI_FOCUS_BROWSER, 'j',	gui_browser_cursor_down },
	{ GUI_FOCUS_BROWSER, 'k',	gui_browser_cursor_up },
	{ GUI_FOCUS_BROWSER, 0x02,	gui_browser_cursor_pageup }, /* ^B */
	{ GUI_FOCUS_BROWSER, 0x06,	gui_browser_cursor_pagedown }, /* ^F */
	{ GUI_FOCUS_BROWSER, KEY_DOWN,	gui_browser_cursor_down },
	{ GUI_FOCUS_BROWSER, KEY_END,	gui_browser_cursor_bottom },
	{ GUI_FOCUS_BROWSER, KEY_HOME,	gui_browser_cursor_top },
	{ GUI_FOCUS_BROWSER, KEY_NPAGE,	gui_browser_cursor_pagedown },
	{ GUI_FOCUS_BROWSER, KEY_PPAGE,	gui_browser_cursor_pageup },
	{ GUI_FOCUS_BROWSER, KEY_UP,	gui_browser_cursor_up },

	/* Keyboard bindings for the playlist */
	{ GUI_FOCUS_PLAYQ, ' ',		gui_playq_cursor_pagedown },
	{ GUI_FOCUS_PLAYQ, 'f',		gui_playq_fullpath },
	{ GUI_FOCUS_PLAYQ, 'G',		gui_playq_cursor_bottom },
	{ GUI_FOCUS_PLAYQ, 'g',		gui_playq_cursor_top },
	{ GUI_FOCUS_PLAYQ, 'j',		gui_playq_cursor_down },
	{ GUI_FOCUS_PLAYQ, 'k',		gui_playq_cursor_up },
	{ GUI_FOCUS_PLAYQ, 0x02,	gui_playq_cursor_pageup }, /* ^B */
	{ GUI_FOCUS_PLAYQ, 0x06,	gui_playq_cursor_pagedown }, /* ^F */
	{ GUI_FOCUS_PLAYQ, KEY_DOWN,	gui_playq_cursor_down },
	{ GUI_FOCUS_PLAYQ, KEY_END,	gui_playq_cursor_bottom },
	{ GUI_FOCUS_PLAYQ, KEY_HOME,	gui_playq_cursor_top },
	{ GUI_FOCUS_PLAYQ, KEY_NPAGE,	gui_playq_cursor_pagedown },
	{ GUI_FOCUS_PLAYQ, KEY_PPAGE,	gui_playq_cursor_pageup },
	{ GUI_FOCUS_PLAYQ, KEY_UP,	gui_playq_cursor_up },
};
/**
 * @brief Amount of keybindings.
 */
#define NUM_BINDINGS (sizeof kbdbindings / sizeof(struct gui_binding))

void
gui_input_sigmask(void)
{
#ifdef G_THREADS_IMPL_POSIX
	sigset_t sset;

	sigemptyset(&sset);
	sigaddset(&sset, SIGUSR1);
	sigaddset(&sset, SIGUSR2);
	sigaddset(&sset, SIGHUP);
	sigaddset(&sset, SIGTERM);
#ifdef SIGWINCH
	sigaddset(&sset, SIGWINCH);
#endif /* SIGWINCH */
	pthread_sigmask(SIG_BLOCK, &sset, NULL);
#endif /* G_THREADS_IMPL_POSIX */
}

#ifdef G_OS_UNIX
/**
 * @brief Handler of all incoming signals with a custom action.
 */
static void
gui_input_sighandler(int signal)
{
#ifdef BUILD_GUI_SIGWINCH_WRAPPER
	struct winsize ws;
#endif /* BUILD_GUI_SIGWINCH_WRAPPER */

	switch (signal) {
	case SIGUSR1:
		playq_cursong_pause();
		break;
	case SIGUSR2:
		playq_cursong_next();
		break;
	case SIGHUP:
	case SIGTERM:
		gui_input_quit();
		/* NOTREACHED */
#ifdef BUILD_GUI_SIGWINCH_WRAPPER
	case SIGWINCH:
		if (ioctl(STDIN_FILENO, TIOCGWINSZ, &ws) != -1) {
			gui_lock();
			resizeterm(ws.ws_row, ws.ws_col);
			gui_unlock();
			gui_draw_resize();
		}
		break;
#endif /* BUILD_GUI_SIGWINCH_WRAPPER */
	}
}
#endif /* G_OS_UNIX */

void
gui_input_loop(void)
{
	int ch;
	unsigned int i;

#ifdef G_OS_UNIX
	signal(SIGUSR1, gui_input_sighandler);
	signal(SIGUSR2, gui_input_sighandler);
	signal(SIGHUP, gui_input_sighandler);
	signal(SIGTERM, gui_input_sighandler);
#ifdef BUILD_GUI_SIGWINCH_WRAPPER
	signal(SIGWINCH, gui_input_sighandler);
#endif /* BUILD_GUI_SIGWINCH_WRAPPER */
#endif /* G_OS_UNIX */

	for (;;) {
		ch = gui_input_getch();
		gui_msgbar_flush();

		for (i = 0; i < NUM_BINDINGS; i++) {
			/* Let's see if the button matches */
			if (kbdbindings[i].input != ch ||
			    (kbdbindings[i].focus != -1 &&
			     kbdbindings[i].focus != gui_input_curfocus))
				continue;
			
			kbdbindings[i].func();
			break;
		}

		gui_draw_done();
	}
}

int
gui_input_askyesno(const char *question)
{
	char *msg, input;
	const char *yes, *no;
	int ret;

	yes = _("yes");
	no = _("no");

	/* Print the question on screen */
	msg = g_strdup_printf("%s ([%s]/%s): ", question, yes, no);
	gui_msgbar_ask(msg);
	g_free(msg);

	for (;;) {
		input = gui_input_getch();

#ifdef BUILD_NLS
		/* Localized yes/no buttons */
		if (input == yes[0]) {
			ret = 0;
			goto done;
		} else if (input == no[0]) {
			ret = -1;
			goto done;
		}
#endif /* BUILD_NLS */

		/* Default y/n behaviour */
		switch(input) {
		case 'y':
		case 'Y':
		case '\r':
			ret = 0;
			goto done;
		case 'n':
		case 'N':
		case 0x03: /* ^C */
			ret = -1;
			goto done;
		}
	}
done:
	gui_msgbar_flush();
	return (ret);
}

/**
 * @brief Find the offset to where a string should be trimmed to remove
 *        one word or special character sequence, including trailing
 *        whitespace.
 */
static int
gui_input_trimword(GString *gs)
{
	const char *end;

	/* Last character */
	end = (gs->str + gs->len) - 1;

	/* Trim as much trailing whitespace as possible */
	for (;;) {
		if (end < gs->str) return (0);
		if (!isspace(*end)) break;
		end--;
	}

	if (isalnum(*end)) {
		/* Trim alphanumerics */
		do {
			if (--end < gs->str) return (0);
		} while (isalnum(*end));
	} else {
		/* Trim special characters */
		do {
			if (--end < gs->str) return (0);
		} while (!isalnum(*end) && !isspace(*end));
	}

	return (end - gs->str) + 1;
}

char *
gui_input_askstring(const char *question, const char *defstr,
    const char *allowed)
{
	GString *msg;
	unsigned int origlen, newlen;
	int c, clearfirst = 0;
	char *ret = NULL;

	msg = g_string_new(question);
	g_string_append(msg, ": ");
	origlen = msg->len;
	if (defstr != NULL) {
		g_string_append(msg, defstr);
		clearfirst = 1;
	}

	for(;;) {
		gui_msgbar_ask(msg->str);

		switch(c = gui_input_getch()) {
		case '\r':
			goto done;
		case KEY_BACKSPACE:
			clearfirst = 0;
			if (msg->len > origlen) {
				/* Prompt has contents */
				g_string_truncate(msg, msg->len - 1);
			}
			break;
		case 0x03: /* ^C */
			/* Just empty the return */
			g_string_truncate(msg, origlen);
			goto done;
		case 0x15: /* ^U */
			g_string_truncate(msg, origlen);
			break;
		case 0x17: /* ^W */
			clearfirst = 0;
			newlen = gui_input_trimword(msg);
			g_string_truncate(msg, MAX(newlen, origlen));
			break;
		default:
			if ((allowed != NULL && strchr(allowed, c) == NULL) ||
			    g_ascii_iscntrl(c)) {
				/* Character is not allowed */
				break;
			}
			if (clearfirst) {
				g_string_truncate(msg, origlen);
				clearfirst = 0;
			}
			g_string_append_c(msg, c);
		}
	}

done:
	gui_msgbar_flush();

	/* Only return a string when there are contents */
	if (msg->len > origlen)
		ret = g_strdup(msg->str + origlen);

	g_string_free(msg, TRUE);
	return ret;
}
