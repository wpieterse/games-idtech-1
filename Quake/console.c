/*
Copyright (C) 1996-2001 Id Software, Inc.
Copyright (C) 2002-2009 John Fitzgibbons and others
Copyright (C) 2010-2014 QuakeSpasm developers

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.

See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.

*/
// console.c

#include "quakedef.h"
#include "q_ctype.h"

#include <sys/types.h>
#include <time.h>
#include <sys/stat.h>
#include <fcntl.h>
#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

extern qboolean	keydown[256];

int 		con_linewidth;

float		con_cursorspeed = 4;

#define		CON_TEXTSIZE (1024 * 1024) //ericw -- was 65536. johnfitz -- new default size
#define		CON_MINSIZE  16384 //johnfitz -- old default, now the minimum size
#define		CON_MARGIN   1

int		con_buffersize; //johnfitz -- user can now override default

qboolean 	con_forcedup;		// because no entities to refresh

int		con_totallines;		// total lines in console scrollback
int		con_backscroll;		// lines up from bottom to display
int		con_current;		// where next message will be printed
int		con_x;				// offset in current line for next print
char		*con_text = NULL;

typedef struct
{
	int			line;
	int			col;
} conofs_t;

typedef struct
{
	const char	*path;
	conofs_t	begin;
	conofs_t	end;
} conlink_t;

typedef struct
{
	conofs_t		begin;
	conofs_t		end;
} conselection_t;

typedef enum
{
	// Used for link hover/clicking:
	// - picks the character that contains the cursor
	// - rejects areas outside the visible console region
	CT_INSIDE,

	// Used for text selection:
	// - picks the closest edge horizontally, on whichever line contains the cursor vertically
	// - clamps to the margins of the visible console region
	CT_NEAREST,
} contest_t;		// Console hit testing mode

typedef enum
{
	CMS_NOTPRESSED,
	CMS_PRESSED,
	CMS_DRAGGING,
} conmouse_t;

static conlink_t	**con_links = NULL;
static conlink_t	*con_hotlink = NULL;

static conmouse_t		con_mousestate = CMS_NOTPRESSED;
static conselection_t	con_selection;

cvar_t		con_notifytime = {"con_notifytime","3",CVAR_NONE};	//seconds
cvar_t		con_logcenterprint = {"con_logcenterprint", "1", CVAR_NONE}; //johnfitz
cvar_t		con_notifycenter = {"con_notifycenter", "0", CVAR_ARCHIVE};
cvar_t		con_notifyfade = {"con_notifyfade", "0", CVAR_ARCHIVE};
cvar_t		con_notifyfadetime = {"con_notifyfadetime", "0.5", CVAR_ARCHIVE};
cvar_t		con_maxcols = {"con_maxcols", "0", CVAR_ARCHIVE};

char		con_lastcenterstring[1024]; //johnfitz

#define	NUM_CON_TIMES 4
double		con_times[NUM_CON_TIMES];	// realtime time the line was generated
						// for transparent notify lines

int			con_vislines;

qboolean	con_initialized;


/*
================
Con_GetLine
================
*/
static const char *Con_GetLine (int line)
{
	return con_text + (line%con_totallines)*con_linewidth;
}

/*
================
Con_StrLen
================
*/
static size_t Con_StrLen (int line)
{
	const char *text = Con_GetLine (line);
	size_t len = con_linewidth;
	while (len > 0 && (char)(text[len - 1] & 0x7f) == ' ')
		len--;
	return len;
}

static void Con_ScreenToCanvas (int x, int y, int *outx, int *outy)
{
	drawtransform_t	transform;
	float			px, py;

	// screen space to [-1..1]
	px = (x - glx) * 2.f / (float) glwidth - 1.f;
	py = (y - gly) * 2.f / (float) glheight - 1.f;
	py = -py;

	// [-1..1] to console canvas
	Draw_GetCanvasTransform (CANVAS_CONSOLE, &transform);
	px = (px - transform.offset[0]) / transform.scale[0];
	py = (py - transform.offset[1]) / transform.scale[1];
	x = (int) (px + 0.5f);
	y = (int) (py + 0.5f);

	*outx = x;
	*outy = y;
}


/*
================
Con_ScreenToOffset

Converts screen (pixel) coordinates to a console offset
Returns true if the offset is inside the visible portion of the console
================
*/
static qboolean Con_ScreenToOffset (int x, int y, conofs_t *ofs, contest_t testmode)
{
	qboolean ret = true;

	Con_ScreenToCanvas (x, y, &x, &y);

// Start from the bottom of the console
	y = vid.conheight - y;

// Apply rounding
	if (testmode == CT_NEAREST)
		x += 4;

// pixels to characters
	x >>= 3;
	y >>= 3;

// apply margins and scrolling
	x -= CON_MARGIN;
	y -= 2;

	if (testmode == CT_INSIDE)
	{
		if (x < 0 || x >= con_linewidth)
			ret = false;
		if (y < 0 || y >= con_vislines)
			ret = false;
		if (con_backscroll && y < 2)
			ret = false;
	}
	else
	{
		// Allow the cursor to move one character past the end of the line
		// by clamping to con_linewidth instead of con_linewidth - 1
		x = CLAMP (0, x, con_linewidth);

		// Enable selecting the entire bottom line by allowing the cursor
		// to move to the beginning of the line below it (line -1)
		y = CLAMP (-1, y, con_vislines);
		if (y < 0)
			x = 0;

		// Enable selecting the entire line above the backscroll cutoff by
		// allowing the cursor to move to the beginning of the line below it
		if (con_backscroll && y < 2)
		{
			x = 0;
			y = 1;
		}
	}

	y += con_backscroll;
	y = con_current - y;

	ofs->line = y;
	ofs->col = x;

	return ret;
}

/*
================
Con_OfsCompare

Performs a three-way comparison on console offsets
================
*/
static int Con_OfsCompare (const conofs_t *lhs, const conofs_t *rhs)
{
	if (lhs->line != rhs->line)
		return lhs->line - rhs->line;
	return lhs->col - rhs->col;
}

/*
================
Con_OfsInRange

Checks if an offset is within a half-open range
================
*/
static qboolean Con_OfsInRange (const conofs_t *ofs, const conofs_t *begin, const conofs_t *end)
{
	return Con_OfsCompare (ofs, begin) >= 0 && Con_OfsCompare (ofs, end) < 0;
}

/*
================
Con_GetCurrentRange
================
*/
static void Con_GetCurrentRange (conofs_t *begin, conofs_t *end)
{
	begin->line = con_current - con_totallines + 1;
	begin->col = 0;
	end->line = con_current + 1;
	end->col = 0;
}

/*
================
Con_IntersectRanges
================
*/
static qboolean Con_IntersectRanges (conofs_t *begin, conofs_t *end, const conofs_t *selbegin, const conofs_t *selend)
{
	if (Con_OfsCompare (selend, begin) <= 0)
		return false;
	if (Con_OfsCompare (end, selbegin) <= 0)
		return false;

	if (Con_OfsCompare (begin, selbegin) < 0)
		*begin = *selbegin;
	if (Con_OfsCompare (selend, end) < 0)
		*end = *selend;

	return true;
}

/*
================
Con_GetLinkAtOfs

Returns the link at the given offset, if any, or NULL otherwise
================
*/
static conlink_t *Con_GetLinkAtOfs (const conofs_t *ofs)
{
	size_t lo, hi;

// find the first link that ends after the offset
	lo = 0;
	hi = VEC_SIZE (con_links);
	while (lo < hi)
	{
		size_t mid = (lo + hi) / 2;
		if (Con_OfsCompare (ofs, &con_links[mid]->end) >= 0)
			lo = mid + 1;
		else
			hi = mid;
	}

	if (lo == VEC_SIZE (con_links))
		return NULL;

	if (Con_OfsCompare (ofs, &con_links[lo]->begin) >= 0)
		return con_links[lo];

	return NULL;
}

/*
================
Con_GetLinkAtPixel

Returns the link at the given pixel coordinates, if any, or NULL otherwise
================
*/
static conlink_t *Con_GetLinkAtPixel (int x, int y)
{
	conofs_t ofs;
	if (!Con_ScreenToOffset (x, y, &ofs, CT_INSIDE))
		return NULL;
	return Con_GetLinkAtOfs (&ofs);
}

/*
================
Con_SetHotLink

Changes the current hot link and updates the mouse cursor
================
*/
static void Con_SetHotLink (conlink_t *link)
{
	if (link == con_hotlink)
		return;
	con_hotlink = link;
}

/*
================
Con_GetMousePos

Computes the console offset corresponding to the current mouse position
Returns true if the offset is inside the visible portion of the console
================
*/
static qboolean Con_GetMousePos (conofs_t *ofs, contest_t testmode)
{
	int x, y;
	SDL_GetMouseState (&x, &y);
	return Con_ScreenToOffset (x, y, ofs, testmode);
}

/*
================
Con_GetMouseLink

Returns the link at the current mouse position, if any, or NULL otherwise
================
*/
static conlink_t *Con_GetMouseLink (void)
{
	conofs_t ofs;
	if (Con_GetMousePos (&ofs, CT_INSIDE))
		return Con_GetLinkAtOfs (&ofs);
	return NULL;
}

/*
================
Con_ClearSelection
================
*/
static void Con_ClearSelection (void)
{
	memset (&con_selection, 0, sizeof (con_selection));
}

/*
================
Con_HasSelection
================
*/
static qboolean Con_HasSelection (void)
{
	return Con_OfsCompare (&con_selection.begin, &con_selection.end) != 0;
}

/*
================
Con_GetNormalizedSelection
================
*/
static qboolean Con_GetNormalizedSelection (conofs_t *begin, conofs_t *end)
{
	conofs_t	*selbegin = &con_selection.begin;
	conofs_t	*selend = &con_selection.end;
	conofs_t	tbegin, tend;

	if (Con_OfsCompare (selbegin, selend) > 0)
	{
		conofs_t *tmp = selbegin;
		selbegin = selend;
		selend = tmp;
	}
	*begin = *selbegin;
	*end = *selend;

	Con_GetCurrentRange (&tbegin, &tend);

	return Con_IntersectRanges (begin, end, &tbegin, &tend);
}

/*
================
Con_SetMouseState
================
*/
static void Con_SetMouseState (conmouse_t state)
{
	if (con_mousestate == state)
		return;

	switch (state)
	{
	case CMS_PRESSED:
		Con_GetMousePos (&con_selection.begin, CT_NEAREST);
		con_selection.end = con_selection.begin;
		break;

	case CMS_DRAGGING:
		Con_SetHotLink (NULL);
		VID_SetMouseCursor (MOUSECURSOR_IBEAM);
		break;

	case CMS_NOTPRESSED:
		if (con_mousestate != CMS_DRAGGING && con_hotlink && !Sys_Explore (con_hotlink->path))
			S_LocalSound ("misc/menu2.wav");
		break;

	default:
		break;
	}

	con_mousestate = state;
	Con_ForceMouseMove ();
}

/*
================
Con_Mousemove
Mouse movement callback
================
*/
void Con_Mousemove (int x, int y)
{
	if (con_mousestate == CMS_NOTPRESSED)
	{
		Con_SetHotLink (Con_GetLinkAtPixel (x, y));
		VID_SetMouseCursor (con_hotlink ? MOUSECURSOR_HAND : MOUSECURSOR_DEFAULT);
	}
	else
	{
		Con_ScreenToOffset (x, y, &con_selection.end, CT_NEAREST);
		if (Con_OfsCompare (&con_selection.begin, &con_selection.end) != 0)
			Con_SetMouseState (CMS_DRAGGING);
	}
}

/*
================
Con_ForceMouseMove
================
*/
void Con_ForceMouseMove (void)
{
	int x, y;
	SDL_GetMouseState (&x, &y);
	Con_Mousemove (x, y);
}

/*
================
Con_UpdateMouseState
================
*/
static void Con_UpdateMouseState (void)
{
	if (key_dest != key_console)
	{
		Con_SetHotLink (NULL);
		Con_SetMouseState (CMS_NOTPRESSED);
		Con_ClearSelection ();
		return;
	}

	if (!keydown[K_MOUSE1])
		Con_SetMouseState (CMS_NOTPRESSED);
	else if (con_mousestate == CMS_NOTPRESSED)
		Con_SetMouseState (CMS_PRESSED);
}

/*
================
Con_Quakebar -- johnfitz -- returns a bar of the desired length, but never wider than the console

includes a newline, unless len >= con_linewidth.
================
*/
const char *Con_Quakebar (int len)
{
	static char bar[42];
	int i;

	len = q_min(len, (int)sizeof(bar) - 2);
	len = q_min(len, con_linewidth);

	bar[0] = '\35';
	for (i = 1; i < len - 1; i++)
		bar[i] = '\36';
	bar[len-1] = '\37';

	if (len < con_linewidth)
	{
		bar[len] = '\n';
		bar[len+1] = 0;
	}
	else
		bar[len] = 0;

	return bar;
}

/*
================
Con_ToggleConsole_f
================
*/
extern int history_line; //johnfitz

void Con_ToggleConsole_f (void)
{
	if (key_dest == key_console/* || (key_dest == key_game && con_forcedup)*/)
	{
		key_lines[edit_line][1] = 0;	// clear any typing
		key_linepos = 1;
		con_backscroll = 0; //johnfitz -- toggleconsole should return you to the bottom of the scrollback
		history_line = edit_line; //johnfitz -- it should also return you to the bottom of the command history
		key_tabhint[0] = '\0';			// clear tab hint
		Con_SetHotLink (NULL);

		if (cls.state == ca_connected)
		{
			IN_Activate();
			key_dest = key_game;
		}
		else
		{
			M_Menu_Main_f ();
		}
	}
	else
	{
		IN_DeactivateForConsole();
		key_dest = key_console;
	}

	SCR_EndLoadingPlaque ();
	memset (con_times, 0, sizeof(con_times));
}

/*
================
Con_Clear_f
================
*/
static void Con_Clear_f (void)
{
	size_t i;

	if (con_text)
		Q_memset (con_text, ' ', con_buffersize); //johnfitz -- con_buffersize replaces CON_TEXTSIZE

	con_backscroll = 0; //johnfitz -- if console is empty, being scrolled up is confusing

	Con_SetHotLink (NULL);
	for (i = 0; i < VEC_SIZE (con_links); i++)
		free (con_links[i]);
	VEC_CLEAR (con_links);
}

/*
================
Con_CopySelectionToClipboard
================
*/
qboolean Con_CopySelectionToClipboard (void)
{
	conofs_t selbegin, selend;
	conofs_t cursor, eol;
	char *qtext = NULL;
	char *utf8 = NULL;
	size_t maxsize;

	S_LocalSound ("misc/menu2.wav");

	// Get forward selection range
	if (!Con_GetNormalizedSelection (&selbegin, &selend))
		return false;

	// Iterate through all lines in the selection
	for (cursor = selbegin; Con_OfsCompare (&cursor, &selend) <= 0; cursor.line++, cursor.col = 0)
	{
		const char *text = Con_GetLine (cursor.line);
		eol.line = cursor.line;
		eol.col = Con_StrLen (cursor.line);
		if (cursor.line == selend.line)
			eol.col = q_min (eol.col, selend.col);
		Vec_Append ((void **)&qtext, 1, text + cursor.col, eol.col - cursor.col);
		if (eol.line != selend.line)
			VEC_PUSH (qtext, '\n');
	}
	VEC_PUSH (qtext, '\0');

	// Convert to UTF-8
	maxsize = UTF8_FromQuake (NULL, 0, qtext);
	utf8 = (char *) malloc (maxsize);
	UTF8_FromQuake (utf8, maxsize, qtext);

	// Copy the UTF-8 text to clipboard
	SDL_SetClipboardText (utf8);

	// Clean up temporary buffers
	free (utf8);
	VEC_FREE (qtext);

	Con_ClearSelection ();

	return true;
}

/*
================
Con_Dump_f -- johnfitz -- adapted from quake2 source
================
*/
static void Con_Dump_f (void)
{
	int		l, x;
	const char	*line;
	FILE	*f;
	char	buffer[1024];
	char	relname[MAX_OSPATH];
	char	name[MAX_OSPATH];

	q_strlcpy (relname, Cmd_Argc () >= 2 ? Cmd_Argv (1) : "condump.txt", sizeof (relname));
	COM_AddExtension (relname, ".txt", sizeof (relname));
	q_snprintf (name, sizeof(name), "%s/%s", com_gamedir, relname);
	f = Sys_fopen (name, "w");
	if (!f)
	{
		Con_Printf ("ERROR: couldn't open file %s.\n", relname);
		return;
	}

	// skip initial empty lines
	for (l = con_current - con_totallines + 1; l <= con_current; l++)
	{
		line = con_text + (l % con_totallines)*con_linewidth;
		for (x = 0; x < con_linewidth; x++)
			if (line[x] != ' ')
				break;
		if (x != con_linewidth)
			break;
	}

	// write the remaining lines
	buffer[con_linewidth] = 0;
	for ( ; l <= con_current; l++)
	{
		line = con_text + (l%con_totallines)*con_linewidth;
		strncpy (buffer, line, con_linewidth);
		for (x = con_linewidth - 1; x >= 0; x--)
		{
			if (buffer[x] == ' ')
				buffer[x] = 0;
			else
				break;
		}
		for (x = 0; buffer[x]; x++)
			buffer[x] &= 0x7f;

		fprintf (f, "%s\n", buffer);
	}

	fclose (f);
	Con_SafePrintf ("Dumped console text to ");
	Con_LinkPrintf (name, "%s", relname);
	Con_SafePrintf (".\n");
}

/*
================
Con_ClearNotify
================
*/
void Con_ClearNotify (void)
{
	int		i;

	for (i = 0; i < NUM_CON_TIMES; i++)
		con_times[i] = 0;
}


/*
================
Con_MessageMode_f
================
*/
static void Con_MessageMode_f (void)
{
	if (cls.state != ca_connected || cls.demoplayback)
		return;
	chat_team = false;
	key_dest = key_message;
}

/*
================
Con_MessageMode2_f
================
*/
static void Con_MessageMode2_f (void)
{
	if (cls.state != ca_connected || cls.demoplayback)
		return;
	chat_team = true;
	key_dest = key_message;
}


void Con_RecalcOffset (conofs_t *ofs, int oldnumlines)
{
	ofs->col = q_min (ofs->col, con_linewidth);
	ofs->line += con_totallines - 1 - oldnumlines;
}


/*
================
Con_CheckResize

If the line width has changed, reformat the buffer.
================
*/
void Con_CheckResize (void)
{
	int	i, j, width, oldwidth, oldtotallines, numlines, numchars;
	char	*tbuf; //johnfitz -- tbuf no longer a static array
	int mark; //johnfitz

	width = (vid.conwidth >> 3) - CON_MARGIN*2; //johnfitz -- use vid.conwidth instead of vid.width

	if (width == con_linewidth)
		return;

	oldwidth = con_linewidth;
	con_linewidth = width;
	oldtotallines = con_totallines;
	con_totallines = con_buffersize / con_linewidth; //johnfitz -- con_buffersize replaces CON_TEXTSIZE
	numlines = oldtotallines;

	if (con_totallines < numlines)
		numlines = con_totallines;

	numchars = oldwidth;

	if (con_linewidth < numchars)
		numchars = con_linewidth;

	mark = Hunk_LowMark (); //johnfitz
	tbuf = (char *) Hunk_Alloc (con_buffersize); //johnfitz

	Q_memcpy (tbuf, con_text, con_buffersize);//johnfitz -- con_buffersize replaces CON_TEXTSIZE
	Q_memset (con_text, ' ', con_buffersize);//johnfitz -- con_buffersize replaces CON_TEXTSIZE

	for (i = 0; i < numlines; i++)
	{
		for (j = 0; j < numchars; j++)
		{
			con_text[(con_totallines - 1 - i) * con_linewidth + j] =
					tbuf[((con_current - i + oldtotallines) % oldtotallines) * oldwidth + j];
		}
	}

	Hunk_FreeToLowMark (mark); //johnfitz

	for (i = 0; i < (int) VEC_SIZE (con_links); i++)
	{
		conlink_t *link = con_links[i];
		Con_RecalcOffset (&link->begin, con_current);
		Con_RecalcOffset (&link->end, con_current);
	}

	Con_ClearNotify ();

	con_backscroll = 0;
	con_current = con_totallines - 1;
}


/*
================
Con_Scroll
================
*/
void Con_Scroll (int lines)
{
	if (!lines)
		return;

	con_backscroll += lines;

	if (lines > 0)
	{
		if (con_backscroll > con_totallines - (vid.height>>3) - 1)
			con_backscroll = con_totallines - (vid.height>>3) - 1;
	}
	else
	{
		if (con_backscroll < 0)
			con_backscroll = 0;
	}

	Con_ForceMouseMove ();
}


/*
================
Con_Init
================
*/
void Con_Init (void)
{
	int i;

	//johnfitz -- user settable console buffer size
	i = COM_CheckParm("-consize");
	if (i && i < com_argc-1) {
		con_buffersize = Q_atoi(com_argv[i+1])*1024;
		if (con_buffersize < CON_MINSIZE)
			con_buffersize = CON_MINSIZE;
	}
	else
		con_buffersize = CON_TEXTSIZE;
	//johnfitz

	con_text = (char *) Hunk_AllocName (con_buffersize, "context");//johnfitz -- con_buffersize replaces CON_TEXTSIZE
	Q_memset (con_text, ' ', con_buffersize);//johnfitz -- con_buffersize replaces CON_TEXTSIZE
	con_linewidth = -1;

	//johnfitz -- no need to run Con_CheckResize here
	con_linewidth = 78;
	con_totallines = con_buffersize / con_linewidth;//johnfitz -- con_buffersize replaces CON_TEXTSIZE
	con_backscroll = 0;
	con_current = con_totallines - 1;
	//johnfitz

	Con_Printf ("Console initialized.\n");

	Cvar_RegisterVariable (&con_notifytime);
	Cvar_RegisterVariable (&con_notifycenter);
	Cvar_RegisterVariable (&con_notifyfade);
	Cvar_RegisterVariable (&con_notifyfadetime);
	Cvar_RegisterVariable (&con_logcenterprint); //johnfitz
	Cvar_RegisterVariable (&con_maxcols);

	Cmd_AddCommand ("toggleconsole", Con_ToggleConsole_f);
	Cmd_AddCommand ("messagemode", Con_MessageMode_f);
	Cmd_AddCommand ("messagemode2", Con_MessageMode2_f);
	Cmd_AddCommand ("clear", Con_Clear_f);
	Cmd_AddCommand ("condump", Con_Dump_f); //johnfitz
	con_initialized = true;
}


/*
===============
Con_Linefeed
===============
*/
static void Con_Linefeed (void)
{
	//johnfitz -- improved scrolling
	if (con_backscroll)
		con_backscroll++;
	if (con_backscroll > con_totallines - (glheight>>3) - 1)
		con_backscroll = con_totallines - (glheight>>3) - 1;
	//johnfitz

	con_x = 0;
	con_current++;
	Q_memset (&con_text[(con_current%con_totallines)*con_linewidth], ' ', con_linewidth);
}

/*
================
Con_Print

Handles cursor positioning, line wrapping, etc
All console printing must go through this in order to be logged to disk
If no console is visible, the notify window will pop up.
================
*/
static void Con_Print (const char *txt)
{
	int		y;
	int		c, l;
	static int	cr;
	int		mask;
	qboolean	boundary, skipnotify;

	//con_backscroll = 0; //johnfitz -- better console scrolling

	if (txt[0] == 1)
	{
		mask = 128;		// go to colored text
		S_LocalSound ("misc/talk.wav");	// play talk wav
		txt++;
	}
	else if (txt[0] == 2)
	{
		mask = 128;		// go to colored text
		txt++;
	}
	else
		mask = 0;

	boundary = true;
	skipnotify = false;
	if (!Q_strncmp (txt, "[skipnotify]", 12))
	{
		skipnotify = true;
		txt += 12;
	}

	while ( (c = *txt) )
	{
		if (c <= ' ')
		{
			boundary = true;
		}
		else if (boundary)
		{
			// count word length
			for (l = 0; l < con_linewidth; l++)
				if (txt[l] <= ' ')
					break;

			// word wrap
			if (l != con_linewidth && (con_x + l > con_linewidth))
				con_x = 0;

			boundary = false;
		}

		txt++;

		if (cr)
		{
			con_current--;
			cr = false;
		}

		if (!con_x)
		{
			Con_Linefeed ();
		// mark time for transparent overlay
			if (con_current >= 0)
				con_times[con_current % NUM_CON_TIMES] = skipnotify ? 0 : realtime;
		}

		switch (c)
		{
		case '\n':
			con_x = 0;
			break;

		case '\r':
			con_x = 0;
			cr = 1;
			break;

		default:	// display character and advance
			y = con_current % con_totallines;
			con_text[y*con_linewidth+con_x] = c | mask;
			con_x++;
			if (con_x >= con_linewidth)
				con_x = 0;
			break;
		}
	}
}


// borrowed from uhexen2 by S.A. for new procs, LOG_Init, LOG_Close

static char	logfilename[MAX_OSPATH];	// current logfile name
static int	log_fd = -1;			// log file descriptor

/*
================
Con_DebugLog

Writes msg to log if -condebug was specified on the command line

Note: msg is expected to be in UTF-8, not Quake charset
================
*/
void Con_DebugLog(const char *msg)
{
	if (log_fd == -1)
		return;

	if (write(log_fd, msg, strlen(msg)) < 0)
	{
		close (log_fd);
		log_fd = -1;
		fprintf (stderr, "Error writing to log file\n");
	}
}


/*
================
Con_StripControlPrefixes
================
*/
static const char *Con_StripControlPrefixes (const char *txt)
{
	// colored text
	if (txt[0] == 1 || txt[0] == 2)
		txt++;

	// [skipnotify]
	if (!Q_strncmp (txt, "[skipnotify]", 12))
		txt += 12;

	return txt;
}


/*
================
Con_Printf

Handles cursor positioning, line wrapping, etc
================
*/
#define	MAXPRINTMSG	4096
void Con_Printf (const char *fmt, ...)
{
	va_list		argptr;
	char		msg[MAXPRINTMSG];
	static qboolean	inupdate;

	va_start (argptr, fmt);
	q_vsnprintf (msg, sizeof(msg), fmt, argptr);
	va_end (argptr);

// also echo to debugging console
	Sys_Printf ("%s", Con_StripControlPrefixes (msg));

	if (!con_initialized)
		return;

	if (cls.state == ca_dedicated)
		return;		// no graphics mode

// write it to the scrollable buffer
	Con_Print (msg);

// update the screen if the console is displayed
	if (cls.signon != SIGNONS && !scr_disabled_for_loading )
	{
	// protect against infinite loop if something in SCR_UpdateScreen calls
	// Con_Printd
		if (!inupdate)
		{
			inupdate = true;
			SCR_UpdateScreen ();
			inupdate = false;
		}
	}
}

/*
================
Con_DWarning -- ericw
 
same as Con_Warning, but only prints if "developer" cvar is set.
use for "exceeds standard limit of" messages, which are only relevant for developers
targetting vanilla engines
================
*/
void Con_DWarning (const char *fmt, ...)
{
	va_list		argptr;
	char		msg[MAXPRINTMSG];

	if (!developer.value)
		return;			// don't confuse non-developers with techie stuff...

	va_start (argptr, fmt);
	q_vsnprintf (msg, sizeof(msg), fmt, argptr);
	va_end (argptr);

	Con_SafePrintf ("\x02Warning: ");
	Con_Printf ("%s", msg);
}

/*
================
Con_Warning -- johnfitz -- prints a warning to the console
================
*/
void Con_Warning (const char *fmt, ...)
{
	va_list		argptr;
	char		msg[MAXPRINTMSG];

	va_start (argptr, fmt);
	q_vsnprintf (msg, sizeof(msg), fmt, argptr);
	va_end (argptr);

	Con_SafePrintf ("\x02Warning: ");
	Con_Printf ("%s", msg);
}

/*
================
Con_DPrintf

A Con_Printf that only shows up if the "developer" cvar is set
================
*/
void Con_DPrintf (const char *fmt, ...)
{
	va_list		argptr;
	char		msg[MAXPRINTMSG];

	if (!developer.value)
		return;			// don't confuse non-developers with techie stuff...

	va_start (argptr, fmt);
	q_vsnprintf (msg, sizeof(msg), fmt, argptr);
	va_end (argptr);

	Con_SafePrintf ("%s", msg); //johnfitz -- was Con_Printf
}

/*
================
Con_DPrintf2 -- johnfitz -- only prints if "developer" >= 2

currently not used
================
*/
void Con_DPrintf2 (const char *fmt, ...)
{
	va_list		argptr;
	char		msg[MAXPRINTMSG];

	if (developer.value >= 2)
	{
		va_start (argptr, fmt);
		q_vsnprintf (msg, sizeof(msg), fmt, argptr);
		va_end (argptr);
		Con_Printf ("%s", msg);
	}
}


/*
==================
Con_LinkPrintf

Prints text that opens a link when clicked
==================
*/
void Con_LinkPrintf (const char *addr, const char *fmt, ...)
{
	conlink_t	*link;
	size_t		len;
	va_list		argptr;
	char		msg[MAXPRINTMSG];
	char		*text;

	len = strlen (addr);
	link = (conlink_t *) malloc (sizeof (conlink_t) + len + 1);
	if (!link)
		Sys_Error ("Con_LinkPrintf: out of memory on %" SDL_PRIu64 "u bytes", (uint64_t)(sizeof (conlink_t) + len + 1));
	
	memcpy (link + 1, addr, len + 1);
	link->path			= (const char *)(link + 1);
	link->begin.line	= con_current;
	link->begin.col		= con_x;
	link->end			= link->begin;

	va_start (argptr, fmt);
	q_vsnprintf (msg, sizeof(msg), fmt, argptr);
	va_end (argptr);

	Con_SafePrintf ("\x02%s", msg);

	link->end.line	= con_current;
	link->end.col	= con_x;
	VEC_PUSH (con_links, link);

// Because of wrapping our text might actually start on the next line, so we skip leading spaces
	text = con_text + (link->begin.line % con_totallines)*con_linewidth + link->begin.col;
	while (Con_OfsCompare (&link->begin, &link->end) < 0)
	{
		if ((*text & 0x7f) != ' ')
			break;
		text++;
		if (++link->begin.col == con_linewidth)
		{
			link->begin.col = 0;
			link->begin.line++;
		}
	}
}


/*
==================
Con_SafePrintf

Okay to call even when the screen can't be updated
==================
*/
void Con_SafePrintf (const char *fmt, ...)
{
	va_list		argptr;
	char		msg[MAXPRINTMSG];
	int		temp;

	va_start (argptr, fmt);
	q_vsnprintf (msg, sizeof(msg), fmt, argptr);
	va_end (argptr);

	temp = scr_disabled_for_loading;
	scr_disabled_for_loading = true;
	Con_Printf ("%s", msg);
	scr_disabled_for_loading = temp;
}

/*
================
Con_CenterPrintf -- johnfitz -- pad each line with spaces to make it appear centered
================
*/
void Con_CenterPrintf (int linewidth, const char *fmt, ...) FUNC_PRINTF(2,3);
void Con_CenterPrintf (int linewidth, const char *fmt, ...)
{
	va_list	argptr;
	char	msg[MAXPRINTMSG]; //the original message
	char	line[MAXPRINTMSG]; //one line from the message
	char	spaces[21]; //buffer for spaces
	char	*src, *dst;
	int		len, s;

	va_start (argptr, fmt);
	q_vsnprintf (msg, sizeof(msg), fmt, argptr);
	va_end (argptr);

	linewidth = q_min(linewidth, con_linewidth);
	for (src = msg; *src; )
	{
		dst = line;
		while (*src && *src != '\n')
			*dst++ = *src++;
		*dst = 0;
		if (*src == '\n')
			src++;

		len = strlen(line);
		if (len < linewidth)
		{
			s = (linewidth-len)/2;
			memset (spaces, ' ', s);
			spaces[s] = 0;
			Con_Printf ("%s%s\n", spaces, line);
		}
		else
			Con_Printf ("%s\n", line);
	}
}

/*
==================
Con_LogCenterPrint -- johnfitz -- echo centerprint message to the console
==================
*/
void Con_LogCenterPrint (const char *str)
{
	if (!strcmp(str, con_lastcenterstring))
		return; //ignore duplicates

	if (cl.gametype == GAME_DEATHMATCH && con_logcenterprint.value != 2)
		return; //don't log in deathmatch

	strcpy(con_lastcenterstring, str);

	if (con_logcenterprint.value)
	{
		qboolean trailing_newline = *str && str[strlen (str) - 1] == '\n';
		Con_Printf ("%s", Con_Quakebar(40));
		Con_CenterPrintf (40, trailing_newline ? "%s" : "%s\n", str);
		Con_Printf ("%s", Con_Quakebar(40));
		Con_ClearNotify ();
	}
}

/*
==============================================================================

	TAB COMPLETION

==============================================================================
*/

//johnfitz -- tab completion stuff
//unique defs
char key_tabpartial[MAXCMDLINE];
typedef struct tab_s
{
	const char	*name;
	const char	*type;
	struct tab_s	*next;
	struct tab_s	*prev;
	int			count;
} tab_t;
tab_t	*tablist;

//defs from elsewhere
extern	cmd_function_t	*cmd_functions;
#define	MAX_ALIAS_NAME	32
typedef struct cmdalias_s
{
	struct cmdalias_s	*next;
	char	name[MAX_ALIAS_NAME];
	char	*value;
} cmdalias_t;
extern	cmdalias_t	*cmd_alias;

/*
============
Con_AddToTabList -- johnfitz

tablist is a doubly-linked loop, alphabetized by name
============
*/

// bash_partial is the string that can be expanded,
// aka Linux Bash shell. -- S.A.
static char	bash_partial[80];
static qboolean	bash_singlematch;

void Con_AddToTabList (const char *name, const char *partial, const char *type)
{
	tab_t	*t,*insert;
	char	*i_bash, *i_bash2;
	const char *i_name, *i_name2;
	int		len, mark;

	if (!Con_Match (name, partial))
		return;

	if (!*bash_partial && bash_singlematch)
	{
		q_strlcpy (bash_partial, name, sizeof (bash_partial));
	}
	else
	{
		bash_singlematch = 0;
		i_bash = q_strcasestr (bash_partial, partial);
		i_name = q_strcasestr (name, partial);
		SDL_assert (i_bash);
		SDL_assert (i_name);
		if (i_name && i_bash)
		{
			i_bash2 = i_bash;
			i_name2 = i_name;
			// find max common between bash_partial and name (right side)
			while (*i_bash && q_toupper (*i_bash) == q_toupper (*i_name))
			{
				i_bash++;
				i_name++;
			}
			*i_bash = 0;
			// find max common between bash_partial and name (left side)
			while (i_bash2 != bash_partial && i_name2 != name &&
				q_toupper (i_bash2[-1]) == q_toupper (i_name2[-1]))
			{
				i_bash2--;
				i_name2--;
			}
			if (i_bash2 != bash_partial)
				memmove (bash_partial, i_bash2, strlen (i_bash2) + 1);
		}
	}

	mark = Hunk_LowMark ();
	len = strlen (name);
	t = (tab_t *) Hunk_AllocName (sizeof (tab_t) + len + 1, "tablist");
	memcpy (t + 1, name, len + 1);
	t->name = (const char *) (t + 1);
	t->type = type;
	t->count = 1;

	if (!tablist) //create list
	{
		tablist = t;
		t->next = t;
		t->prev = t;
	}
	else if (q_strnaturalcmp (name, tablist->name) < 0) //insert at front
	{
		t->next = tablist;
		t->prev = tablist->prev;
		t->next->prev = t;
		t->prev->next = t;
		tablist = t;
	}
	else //insert later
	{
		insert = tablist;
		do
		{
			int cmp = q_strnaturalcmp (name, insert->name);
			if (!cmp && !strcmp (name, insert->name)) // avoid duplicates
			{
				Hunk_FreeToLowMark (mark);
				insert->count++;
				return;
			}
			if (cmp < 0)
				break;
			insert = insert->next;
		} while (insert != tablist);

		t->next = insert;
		t->prev = insert->prev;
		t->next->prev = t;
		t->prev->next = t;
	}
}

/*
============
Con_Match
============
*/
qboolean Con_Match (const char *str, const char *partial)
{
	return q_strcasestr (str, partial) != NULL;
}

/*
============
ParseCommand
============
*/
static const char *ParseCommand (void)
{
	char buf[MAXCMDLINE];
	const char *str = key_lines[edit_line] + 1; 
	const char *end = str + key_linepos - 1;
	const char *ret = str;
	const char *quote = NULL;

	while (*str && str != end)
	{
		char c = *str++;
		if (c == '\"')
		{
			if (!quote)
			{
				quote = ret; // save previous command boundary
				ret = str; // new command
			}
			else
			{
				ret = quote; // restore saved cursor
				quote = NULL;
			}
		}
		else if (c == ';')
			ret = str;
		else if (!quote && c == '/' && *str == '/')
			break;
	}

	while (*ret == ' ')
		ret++;

	q_strlcpy (buf, ret, sizeof (buf));
	if ((uintptr_t) (end - ret) < sizeof (buf))
		buf[end - ret] = '\0';
	end = buf + strlen (buf);

	Cmd_TokenizeString (buf);
	// last arg should always be the one we're trying to complete,
	// so we add a new empty one if the command ends with a space
	if (end != buf && end[-1] == ' ')
		Cmd_AddArg ("");

	return ret;
}

static qboolean CompleteFileList (const char *partial, void *param)
{
	filelist_item_t *file, **list = (filelist_item_t **) param;
	for (file = *list; file; file = file->next)
		Con_AddToTabList (file->name, partial, NULL);
	return true;
}

static qboolean CompleteClassnames (const char *partial, void *unused)
{
	extern edict_t *sv_player;
	qcvm_t	*oldvm;
	edict_t	*ed;
	int		i;

	if (!sv.active)
		return true;
	PR_PushQCVM (&sv.qcvm, &oldvm);

	for (i = 1, ed = NEXT_EDICT (qcvm->edicts); i < qcvm->num_edicts; i++, ed = NEXT_EDICT (ed))
	{
		const char *name;
		if (ed == sv_player || ed->free || !ed->v.classname)
			continue;
		name = PR_GetString (ed->v.classname);
		if (*name)
			Con_AddToTabList (name, partial, "#");
	}

	PR_PopQCVM (oldvm);

	return true;
}

static qboolean CompleteBindKeys (const char *partial, void *unused)
{
	int i;

	if (Cmd_Argc () > 2)
		return false;

	for (i = 0; i < MAX_KEYS; i++)
	{
		const char *name = Key_KeynumToString (i);
		if (strcmp (name, "<UNKNOWN KEYNUM>") != 0)
			Con_AddToTabList (name, partial, keybindings[i]);
	}

	return true;
}

static qboolean CompleteUnbindKeys (const char *partial, void *unused)
{
	int i;

	for (i = 0; i < MAX_KEYS; i++)
	{
		if (keybindings[i])
		{
			const char *name = Key_KeynumToString (i);
			if (strcmp (name, "<UNKNOWN KEYNUM>") != 0)
				Con_AddToTabList (name, partial, keybindings[i]);
		}
	}

	return true;
}

typedef struct arg_completion_type_s
{
	const char		*command;
	qboolean		(*function) (const char *partial, void *param);
	void			*param;
} arg_completion_type_t;

static const arg_completion_type_t arg_completion_types[] =
{
	{ "map",					CompleteFileList,		&extralevels },
	{ "changelevel",			CompleteFileList,		&extralevels },
	{ "game",					CompleteFileList,		&modlist },
	{ "record",					CompleteFileList,		&demolist },
	{ "playdemo",				CompleteFileList,		&demolist },
	{ "timedemo",				CompleteFileList,		&demolist },
	{ "load",					CompleteFileList,		&savelist },
	{ "save",					CompleteFileList,		&savelist },
	{ "sky",					CompleteFileList,		&skylist },
	{ "r_showbboxes_filter",	CompleteClassnames,		NULL },
	{ "bind",					CompleteBindKeys,		NULL },
	{ "unbind",					CompleteUnbindKeys,		NULL },
};

static const int num_arg_completion_types = Q_COUNTOF(arg_completion_types);

/*
============
BuildTabList -- johnfitz
============
*/
static void BuildTabList (const char *partial)
{
	cmdalias_t		*alias;
	cvar_t			*cvar;
	cmd_function_t	*cmd;
	int				i;

	tablist = NULL;

	bash_partial[0] = 0;
	bash_singlematch = 1;

	ParseCommand ();

	if (Cmd_Argc () >= 2)
	{
		cvar = Cvar_FindVar (Cmd_Argv (0));
		if (cvar)
		{
		// cvars can only have one argument
			if (Cmd_Argc () == 2 && cvar->completion)
				cvar->completion (cvar, partial);
			return;
		}

		cmd = Cmd_FindCommand (Cmd_Argv (0));
		if (cmd && cmd->completion)
		{
			cmd->completion (partial);
			return;
		}

		for (i=0; i<num_arg_completion_types; i++)
		{
		// arg_completion contains a command we can complete the arguments
		// for (like "map") and a list of all the maps.
			arg_completion_type_t arg_completion = arg_completion_types[i];

			if (!q_strcasecmp (Cmd_Argv (0), arg_completion.command))
			{
				if (arg_completion.function (partial, arg_completion.param))
					return;
				break;
			}
		}
	}

	if (!*partial)
		return;

	cvar = Cvar_FindVarAfter ("", CVAR_NONE);
	for ( ; cvar ; cvar=cvar->next)
		if (q_strcasestr (cvar->name, partial))
			Con_AddToTabList (cvar->name, partial, "cvar");

	for (cmd=cmd_functions ; cmd ; cmd=cmd->next)
		if (cmd->srctype != src_server && q_strcasestr (cmd->name, partial) && !Cmd_IsReservedName (cmd->name))
			Con_AddToTabList (cmd->name, partial, "command");

	for (alias=cmd_alias ; alias ; alias=alias->next)
		if (q_strcasestr (alias->name, partial))
			Con_AddToTabList (alias->name, partial, "alias");
}

/*
============
Con_FormatTabMatch
============
*/
static void Con_FormatTabMatch (const tab_t *t, char *dst, size_t dstsize)
{
	char tinted[MAXCMDLINE];

	COM_TintSubstring (t->name, bash_partial, tinted, sizeof (tinted));

	if (!t->type)
		q_strlcpy (dst, tinted, dstsize);
	else if (t->type[0] == '#' && !t->type[1])
		q_snprintf (dst, dstsize, "%s (%d)", tinted, t->count);
	else
		q_snprintf (dst, dstsize, "%s (%s)", tinted, t->type);
}

/*
============
Con_PrintTabList
============
*/
static void Con_PrintTabList (void)
{
	char	buf[MAXCMDLINE];
	int		i, maxlen, cols, matches, total;
	tab_t	*t;

// determine maximum item length
	maxlen = 0;
	t = tablist;
	do
	{
		Con_FormatTabMatch (t, buf, sizeof (buf));
		total = (int) strlen (buf);
		maxlen = q_max (maxlen, total);
		t = t->next;
	} while (t != tablist);

// determine number of columns
	if (!maxlen)
		return;
	maxlen += 3;										// indent
	maxlen = q_max (maxlen, 8);							// min width
	maxlen = (maxlen + 3) & ~3;							// round up to multiple of 4
	cols = q_max (con_linewidth, maxlen) / maxlen;
	if (con_maxcols.value >= 1.f)
		cols = q_min (cols, (int) con_maxcols.value);	// apply user limit

// print all matches
	Con_SafePrintf("\n");
	i = matches = total = 0;
	t = tablist;
	do
	{
		Con_FormatTabMatch (t, buf, sizeof (buf));
		if (++i == cols)
		{
			i = 0;
			Con_SafePrintf ("   %s\n", buf);
		}
		else
			Con_SafePrintf ("   %*s", -(maxlen-3), buf);
		if (t->type && t->type[0] == '#' && !t->type[1])
			total += t->count;
		t = t->next;
		++matches;
	} while (t != tablist);
	if (i != 0)
		Con_SafePrintf ("\n");

	if (total > 0)
		Con_SafePrintf ("   %d unique matches (%d total)\n", matches, total);

	Con_SafePrintf("\n");
}

/*
============
Con_TabComplete -- johnfitz
============
*/
void Con_TabComplete (tabcomplete_t mode)
{
	char	partial[MAXCMDLINE];
	const char	*match;
	static char	*c;
	tab_t		*t;
	int		mark, i;

	key_tabhint[0] = '\0';
	if (mode == TABCOMPLETE_AUTOHINT)
	{
		key_tabpartial[0] = '\0';

	// only show completion hint when the cursor is at the end of the line
		if ((size_t)key_linepos >= sizeof (key_lines[edit_line]) || key_lines[edit_line][key_linepos])
			return;
	}

// if editline is empty, return
	if (key_lines[edit_line][1] == 0)
		return;

// get partial string (space -> cursor)
	if (!key_tabpartial[0]) //first time through, find new insert point. (Otherwise, use previous.)
	{
		//work back from cursor until you find a space, quote, semicolon, or prompt
		c = key_lines[edit_line] + key_linepos - 1; //start one space left of cursor
		while (*c!=' ' && *c!='\"' && *c!=';' && c!=key_lines[edit_line])
			c--;
		c++; //start 1 char after the separator we just found
	}
	for (i = 0; c + i < key_lines[edit_line] + key_linepos; i++)
		partial[i] = c[i];
	partial[i] = 0;

//trim trailing space becuase it screws up string comparisons
	if (i > 0 && partial[i-1] == ' ')
		partial[i-1] = 0;

// find a match
	mark = Hunk_LowMark();
	if (!key_tabpartial[0]) //first time through
	{
		q_strlcpy (key_tabpartial, partial, MAXCMDLINE);
		BuildTabList (key_tabpartial);

		if (!tablist)
			return;

		// print list if length > 1 and action is user-initiated
		if (tablist->next != tablist && mode == TABCOMPLETE_USER)
			Con_PrintTabList ();

	//	match = tablist->name;
	// First time, just show maximum matching chars -- S.A.
		match = bash_singlematch ? tablist->name : bash_partial;
	}
	else
	{
		BuildTabList (key_tabpartial);

		if (!tablist)
			return;

		//find current match -- can't save a pointer because the list will be rebuilt each time
		t = tablist;
		match = keydown[K_SHIFT] ? t->prev->name : t->name;
		do
		{
			if (!q_strcasecmp (t->name, partial))
			{
				match = keydown[K_SHIFT] ? t->prev->name : t->next->name;
				break;
			}
			t = t->next;
		} while (t != tablist);
	}

	if (mode == TABCOMPLETE_AUTOHINT)
	{
		size_t len = strlen (partial);
		match = q_strcasestr (match, partial);
		if (match && match[len])
			q_strlcpy (key_tabhint, match + len, sizeof (key_tabhint));
		Hunk_FreeToLowMark (mark);
		key_tabpartial[0] = '\0';
		return;
	}

// insert new match into edit line
	q_strlcpy (partial, match, MAXCMDLINE); //first copy match string
	q_strlcat (partial, key_lines[edit_line] + key_linepos, MAXCMDLINE); //then add chars after cursor
	*c = '\0';	//now copy all of this into edit line
	q_strlcat (key_lines[edit_line], partial, MAXCMDLINE);
	key_linepos = c - key_lines[edit_line] + Q_strlen(match); //set new cursor position
	if (key_linepos >= MAXCMDLINE)
		key_linepos = MAXCMDLINE - 1;

	match = NULL;
	Hunk_FreeToLowMark (mark);

// if cursor is at end of string, let's append a space to make life easier
	if (key_linepos < MAXCMDLINE - 1 &&
	    key_lines[edit_line][key_linepos] == 0 && bash_singlematch)
	{
		key_lines[edit_line][key_linepos] = ' ';
		key_linepos++;
		key_lines[edit_line][key_linepos] = 0;
		key_tabpartial[0] = 0; // restart cycle
	// S.A.: the map argument completion (may be in combination with the bash-style
	// display behavior changes, causes weirdness when completing the arguments for
	// the changelevel command. the line below "fixes" it, although I'm not sure about
	// the reason, yet, neither do I know any possible side effects of it:
		c = key_lines[edit_line] + key_linepos;

		Con_TabComplete (TABCOMPLETE_AUTOHINT);
	}
}

/*
==============================================================================

DRAWING

==============================================================================
*/

/*
================
Con_NotifyAlpha
================
*/
static float Con_NotifyAlpha (double time)
{
	float fade;
	if (!time)
		return 0.f;
	fade = q_max (con_notifyfade.value * con_notifyfadetime.value, 0.f);
	time += con_notifytime.value + fade - realtime;
	if (time <= 0.f)
		return 0.f;
	if (!fade)
		return 1.f;
	time = time / fade;
	return q_min (time, 1.0);
}

/*
================
Con_DrawNotify

Draws the last few lines of output transparently over the game top
================
*/
void Con_DrawNotify (void)
{
	int	i, x, v;
	const char	*text;
	float	alpha;

	GL_SetCanvas (CANVAS_CONSOLE); //johnfitz
	v = vid.conheight; //johnfitz

	for (i = con_current-NUM_CON_TIMES+1; i <= con_current; i++)
	{
		if (i < 0)
			continue;
		alpha = Con_NotifyAlpha (con_times[i % NUM_CON_TIMES]);
		if (alpha <= 0.f)
			continue;
		text = con_text + (i % con_totallines)*con_linewidth;

		clearnotify = 0;

		GL_SetCanvasColor (1.f, 1.f, 1.f, alpha);
		if (con_notifycenter.value)
		{
			int len = con_linewidth;
			while (len > 0 && text[len - 1] == ' ')
				--len;
			for (x = 0; x < len; x++)
				Draw_Character ((con_linewidth - len)*4 + x*8, v + 16, text[x]);
		}
		else
			for (x = 0; x < con_linewidth; x++)
				Draw_Character ((x+1)<<3, v, text[x]);
		GL_SetCanvasColor (1.f, 1.f, 1.f, 1.f);

		v += 8;

		scr_tileclear_updates = 0; //johnfitz
	}

	if (key_dest == key_message)
	{
		clearnotify = 0;

		if (chat_team)
		{
			Draw_String (8, v, "say_team:");
			x = 11;
		}
		else
		{
			Draw_String (8, v, "say:");
			x = 6;
		}

		text = Key_GetChatBuffer();
		i = Key_GetChatMsgLen();
		if (i > con_linewidth - x - 1)
			text += i - con_linewidth + x + 1;

		while (*text)
		{
			Draw_Character (x<<3, v, *text);
			x++;
			text++;
		}

		Draw_Character (x<<3, v, 10 + ((int)(realtime*con_cursorspeed)&1));
		v += 8;

		scr_tileclear_updates = 0; //johnfitz
	}
}

/*
================
Con_DrawInput -- johnfitz -- modified to allow insert editing

The input line scrolls horizontally if typing goes beyond the right edge
================
*/
extern	qpic_t *pic_ovr, *pic_ins; //johnfitz -- new cursor handling

void Con_DrawInput (void)
{
	const char *workline = key_lines[edit_line];
	int	i, ofs, len;

	if (key_dest != key_console && !con_forcedup)
		return;		// don't draw anything

// prestep if horizontally scrolling
	if (key_linepos >= con_linewidth)
		ofs = 1 + key_linepos - con_linewidth;
	else
		ofs = 0;

	len = strlen (workline);

// draw input string
	for (i = 0; i+ofs < len; i++)
		Draw_Character ((i+1)<<3, vid.conheight - 16, workline[i+ofs]);

// draw tab completion hint
	if (key_tabhint[0])
	{
		GL_SetCanvasColor (1.0f, 1.0f, 1.0f, 0.75f);
		for (i = 0; key_tabhint[i] && i+1+len-ofs < con_linewidth+CON_MARGIN*2; i++)
			Draw_Character ((i+1+len-ofs)<<3, vid.conheight - 16, key_tabhint[i] | 0x80);
		GL_SetCanvasColor (1.0f, 1.0f, 1.0f, 1.0f);
	}

// johnfitz -- new cursor handling
	if (!((int)((realtime-key_blinktime)*con_cursorspeed) & 1))
	{
		i = key_linepos - ofs;
		Draw_Pic ((i+1)<<3, vid.conheight - 16, key_insert ? pic_ins : pic_ovr);
	}
}

/*
================
Con_DrawSelectionHighlight
================
*/
void Con_DrawSelectionHighlight (int x, int y, int line)
{
	conofs_t	selbegin, selend;
	conofs_t	begin, end;
	size_t		len;

	if (!Con_GetNormalizedSelection (&selbegin, &selend))
		return;

	len = Con_StrLen (line);
	begin.line = line;
	begin.col = 0;
	end.line = line;
	end.col = len;

	if (!Con_IntersectRanges (&begin, &end, &selbegin, &selend))
		return;

	// Highlight line ends (as in Notepad, Visual Studio etc.)
	if (end.line != selend.line && end.col == len)
		end.col++;

	// ...unless we would end up overlapping the console margin
	end.col = q_min (end.col, con_linewidth);

	Draw_Fill (x + begin.col*8, y, (end.col-begin.col)*8, 8, 220, 1.f);
}

/*
================
Con_DrawConsole -- johnfitz -- heavy revision

Draws the console with the solid background
The typing input line at the bottom should only be drawn if typing is allowed
================
*/
void Con_DrawConsole (int lines, qboolean drawinput)
{
	int	i, x, y, j, sb, rows;
	const char	*text;

	Con_UpdateMouseState ();

	if (lines <= 0)
		return;

	con_vislines = lines * vid.conheight / glheight;
	GL_SetCanvas (CANVAS_CONSOLE);

// draw the background
	Draw_ConsoleBackground ();

// draw the buffer text
	rows = (con_vislines +7)/8;
	y = vid.conheight - rows*8;
	rows -= 2; //for input and version lines
	sb = (con_backscroll) ? 2 : 0;

	for (i = con_current - rows + 1; i <= con_current - sb; i++, y += 8)
	{
		j = i - con_backscroll;
		if (j < 0)
			j = 0;
		text = con_text + (j % con_totallines)*con_linewidth;
		Con_DrawSelectionHighlight (8, y, j);
	}

	y = vid.conheight - (rows+2)*8; // +2 for input and version lines
	for (i = con_current - rows + 1; i <= con_current - sb; i++, y += 8)
	{
		conofs_t ofs;
		j = i - con_backscroll;
		if (j < 0)
			j = 0;
		text = con_text + (j % con_totallines)*con_linewidth;
		ofs.line = j;
		for (x = 0; x < con_linewidth; x++)
		{
			char c = text[x];
			ofs.col = x;
			if (con_hotlink && Con_OfsInRange (&ofs, &con_hotlink->begin, &con_hotlink->end))
			{
				if (keydown[K_MOUSE1])
					c &= 0x7f;
				Draw_Character ((x + 1)<<3, y + 2, '_' | (c & 0x80));
			}
			Draw_Character ((x + 1)<<3, y, c);
		}
	}

// draw scrollback arrows
	if (con_backscroll)
	{
		y += 8; // blank line
		for (x = 0; x < con_linewidth; x += 4)
			Draw_Character ((x + 1)<<3, y, '^');
		y += 8;
	}

// draw the input prompt, user text, and cursor
	if (drawinput)
		Con_DrawInput ();

//draw version number in bottom right
	y += 8;
	text = CONSOLE_TITLE_STRING;
	M_PrintWhite (vid.conwidth - (strlen (text) << 3), y, text);
}


/*
==================
Con_NotifyBox
==================
*/
void Con_NotifyBox (const char *text)
{
	double		t1, t2;
	int		lastkey, lastchar;

// during startup for sound / cd warnings
	Con_Printf ("\n\n%s", Con_Quakebar(40)); //johnfitz
	Con_Printf ("%s", text);
	Con_Printf ("Press a key.\n");
	Con_Printf ("%s", Con_Quakebar(40)); //johnfitz

	IN_DeactivateForConsole();
	key_dest = key_console;

	Key_BeginInputGrab ();
	do
	{
		t1 = Sys_DoubleTime ();
		SCR_UpdateScreen ();
		Sys_SendKeyEvents ();
		Key_GetGrabbedInput (&lastkey, &lastchar);
		Sys_Sleep (16);
		t2 = Sys_DoubleTime ();
		realtime += t2-t1;		// make the cursor blink
	} while (lastkey == -1 && lastchar == -1);
	Key_EndInputGrab ();

	Con_Printf ("\n");
	IN_Activate();
	key_dest = key_game;
	realtime = 0;		// put the cursor back to invisible
}


void LOG_Init (quakeparms_t *parms)
{
	time_t	inittime;
	char	session[24];

	if (!COM_CheckParm("-condebug"))
		return;

	inittime = time (NULL);
	strftime (session, sizeof(session), "%m/%d/%Y %H:%M:%S", localtime(&inittime));
	q_snprintf (logfilename, sizeof(logfilename), "%s/qconsole.log", parms->basedir);

//	unlink (logfilename);

	log_fd = open (logfilename, O_WRONLY | O_CREAT | O_TRUNC, 0666);
	if (log_fd == -1)
	{
		fprintf (stderr, "Error: Unable to create log file %s\n", logfilename);
		return;
	}

	Con_DebugLog (va("LOG started on: %s \n", session));

}

void LOG_Close (void)
{
	if (log_fd == -1)
		return;
	close (log_fd);
	log_fd = -1;
}

