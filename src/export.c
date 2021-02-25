/*-------------------------------------------------------------------------
 *
 * export.c
 *	  a routines for exporting data
 *
 * Portions Copyright (c) 2017-2021 Pavel Stehule
 *
 * IDENTIFICATION
 *	  src/export.c
 *
 *-------------------------------------------------------------------------
 */

#include <errno.h>
#include <string.h>
#include <stdint.h>

#include "pspg.h"
#include "commands.h"
#include "unicode.h"

/*
 * Ensure correct formatting of CSV value. Can returns
 * malloc ed string when value should be quoted.
 */
static char *
csv_format(char *str, int *slen, bool force8bit, bool empty_string_is_null)
{
	char   *ptr = str;
	char   *result;
	bool	needs_quoting = false;
	int		_slen;

	/* Detect NULL symbol ∅ */
	if (!force8bit &&
		*slen == 3 && strncmp(str, "\342\210\205", 3) == 0)
	{
		*slen = 0;
		return NULL;
	}

	if (*slen == 0)
	{
		if (empty_string_is_null)
			return NULL;

		*slen = 2;
		return sstrdup("\"\"");
	}

	_slen = *slen;
	while (_slen > 0)
	{
		int		size;

		if (*ptr == '"' || *ptr == ',' ||
			*ptr == '\t' || *ptr == '\r' || *ptr == '\n')
		{
			needs_quoting = true;
			break;
		}

		size = force8bit ? 1 : utf8charlen(*ptr);

		ptr += size;
		_slen -= size;
	}

	if (!needs_quoting)
		return str;

	result = ptr = smalloc2(*slen * 2 + 2 + 1,
							"CSV format output buffer allocation");

	*ptr++ = '"';

	_slen = *slen;
	*slen = 1;
	while (_slen > 0)
	{
		int		size = force8bit ? 1 : utf8charlen(*ptr);

		if (*str == '"')
			*ptr++ = '"';

		_slen -= size;
		*slen += size;

		while (size--)
			*ptr++ = *str++;
	}

	*ptr++ = '"';
	*ptr = '\0';
	*slen += 1;

	return result;
}

/*
 * Ensure correct format for SQL identifier
 */
static char *
quote_sql_identifier(char *str, int *slen, bool force8bit)
{
	bool	needs_quoting = false;
	char   *ptr, *result;
	int		_slen;

	/* it is quoted already? */
	if (*str == '"')
		return str;

	if (!str || *str == '\0' || *slen == 0)
		return str;

	if (*str != ' ' && (*str < 'a' || *str > 'z'))
		needs_quoting = true;
	else
	{
		ptr = str;
		_slen = *slen;

		while (*ptr && _slen > 0)
		{
			int		size = force8bit ? 1 : utf8charlen(*ptr);

			if (!((*ptr >= 'a' && *ptr <= 'z') ||
				(*ptr >= '0' && *ptr <= '9') ||
				 *ptr == '_'))
			{
				needs_quoting = true;
				break;
			}

			_slen -= size;
			ptr += size;
		}
	}

	if (!needs_quoting)
		return str;

	result = ptr = smalloc2(*slen * 2 + 2 + 1,
							"SQL identifier output buffer allocation");

	*ptr++ = '"';

	_slen = *slen;
	*slen = 1;

	while (_slen > 0)
	{
		int		size = force8bit ? 1 : utf8charlen(*ptr);

		if (*str == '"')
			*ptr++ = '"';

		_slen -= size;
		*slen += size;

		while (size--)
			*ptr++ = *str++;
	}

	*ptr++ = '"';
	*ptr = '\0';
	*slen += 1;

	return result;
}

static char *
quote_sql_literal(char *str,
				  int *slen,
				  bool force8bit,
				  bool empty_string_is_null)
{
	char   *ptr = str;
	char   *result;
	int		_slen = *slen;
	bool	has_dot = false;

	bool	needs_quoting = false;

	if (*slen == 0)
	{
		if (empty_string_is_null)
		{
			*slen = 4;
			return sstrdup("NULL");
		}
		else
		{
			*slen = 2;
			return sstrdup("''");
		}
	}

	if (*slen == 4 &&
		(strncmp(str, "NULL", *slen) == 0 ||
		 strncmp(str, "null", *slen) == 0))
		return str;

	if (!force8bit &&
		*slen == 3 && strncmp(str, "\342\210\205", 3) == 0)
	{
		*slen = 4;
		return sstrdup("NULL");
	}

	while (*ptr && _slen > 0)
	{
		int		size = force8bit ? 1 : utf8charlen(*ptr);

		if (*ptr == '.')
		{
			if (has_dot)
			{
				needs_quoting = true;
				break;
			}
			else
				has_dot = true;
		}
		else if (!(*ptr >= '0' && *ptr <= '9'))
		{
			needs_quoting = true;
			break;
		}

		ptr += size;
		_slen -= size;
	}

	if (!needs_quoting)
		return str;

	result = ptr = smalloc2(*slen * 2 + 2 + 1,
							"SQL literal output buffer allocation");

	*ptr++ = '\'';

	_slen = *slen;
	*slen = 1;

	while (_slen > 0)
	{
		int		size = force8bit ? 1 : utf8charlen(*ptr);

		if (*str == '\'')
			*ptr++ = '\'';

		_slen -= size;
		*slen += size;

		while (size--)
			*ptr++ = *str++;
	}

	*ptr++ = '\'';
	*ptr = '\0';
	*slen += 1;

	return result;
}

/*
 * Iterator over data string with format specified by headline
 */
typedef struct
{
	char	   *row;
	char	   *headline;
	bool		force8bit;
	int			xpos;
} FmtLineIter;

static char *
next_char(FmtLineIter *iter,
		  char *typ,
		  int *size,
		  int *width,
		  int *xpos)
{
	char   *result = iter->row;

	if (!iter->row || !iter->headline)
		return NULL;

	if (*iter->row == '\0' || *iter->headline == '\n')
		return NULL;

	*typ = *(iter->headline);
	*xpos = iter->xpos;

	if (iter->force8bit)
	{
		*size = 1;
		*width = 1;
	}
	else
	{
		*size = utf8charlen(*result);
		*width = utf_dsplen(result);
	}

	iter->row += *size;
	iter->headline += *width;
	iter->xpos += *width;

	return result;
}

typedef struct
{
	FILE	   *fp;
	ClipboardFormat format;
	int			xmin;
	int			xmax;
	bool		copy_line_extended;
	char	   *table_name;
	int			columns;

	bool		force8bit;
	bool		empty_string_is_null;

	int			colno;
	char	  **colnames;
} ExportState;

/*
 * truncate spaces from both ends
 */
static char *
trim_str(char *str, int *size, bool force8bit)
{
	char   *result = NULL;

	while (*str == ' ' && *size > 0)
	{
		str += 1;
		*size -= 1;
	}

	if (*size > 0)
	{
		char   *after_nspc_chr = NULL;

		result = str;

		while (*size > 0)
		{
			int		charlen = force8bit ? 1 : utf8charlen(*str);

			if (*str != ' ')
				after_nspc_chr = str + charlen;

			str = str + charlen;
			*size -= charlen;
		}

		*size = after_nspc_chr - result;
	}

	return result;
}

/*
 * Export one segment of format (decoration or field) to output file.
 */
static bool
process_item(ExportState *expstate,
			 char typ, char *field, int size,
			 int xpos, bool is_colname)
{
	if (INSERT_FORMAT_TYPE(expstate->format))
	{
		errno = 0;

		if (typ == 'N' && !is_colname)
		{
			if (expstate->format == CLIPBOARD_FORMAT_INSERT)
				fputs(");\n", expstate->fp);
			else
			{
				fprintf(expstate->fp, ");\t\t -- %d. %s\n",
					expstate->colno, expstate->colnames[expstate->colno - 1]);
			}
		}
		else if (typ == 'd')
		{
			char   *_field;

			/* Ignore items outer to selected range */
			if (expstate->xmin != -1 &&
				(xpos <= expstate->xmin ||
				 expstate->xmax <= xpos))
			return true;

			if (is_colname)
			{
				if (!expstate->colnames)
				{
					expstate->colnames = smalloc(sizeof(char *) * expstate->columns);
					memset(expstate->colnames, 0, sizeof(char *) * expstate->columns);
				}

				field = trim_str(field, &size, expstate->force8bit);
				_field = quote_sql_identifier(field, &size, expstate->force8bit);

				if (!_field)
					expstate->colnames[expstate->colno] = sstrdup("");
				else if (_field != field)
					expstate->colnames[expstate->colno] = _field;
				else
					expstate->colnames[expstate->colno] = sstrndup(field, size);

				expstate->colno += 1;
			}
			else
			{
				if (expstate->colno == 0)
				{
					errno = 0;

					fputs("INSERT INTO ", expstate->fp);
					fputs(expstate->table_name, expstate->fp);

					if (expstate->colnames)
					{
						int		i;
						bool	is_first = true;

						fputc('(', expstate->fp);

						if (expstate->format == CLIPBOARD_FORMAT_INSERT)
						{
							for (i = 0; i < expstate->columns; i++)
							{
								if (expstate->colnames[i])
								{
									if (!is_first)
										fputs(", ", expstate->fp);
									else
										is_first = false;

									fputs(expstate->colnames[i], expstate->fp);
								}
								else
									break;
							}

							fputc(')', expstate->fp);
						}
						else
						{
							int		indent_spaces;
							int		columns = 0;
							int		loc_colno = 0;

							if (expstate->force8bit)
								indent_spaces = strlen(expstate->table_name) + 1 + 12;
							else
								indent_spaces = utf_string_dsplen(expstate->table_name, SIZE_MAX) + 1 + 12;

							for (i = 0; i < expstate->columns; i++)
							{
								if (expstate->colnames[i])
									columns += 1;
								else
									break;
							}

							for (i = 0; i < expstate->columns; i++)
							{
								if (expstate->colnames[i])
								{
									if (loc_colno > 0)
										fprintf(expstate->fp, "%*.s", indent_spaces, "");

									fputs(expstate->colnames[i], expstate->fp);

									if (loc_colno < columns - 1)
										fprintf(expstate->fp, ",\t\t -- %d.\n", loc_colno + 1);
									else
										fprintf(expstate->fp, ")\t\t -- %d.\n", loc_colno + 1);

									loc_colno += 1;
								}
								else
									break;
							}
						}
					}

					if (expstate->format == CLIPBOARD_FORMAT_INSERT)
						fputs(" VALUES(", expstate->fp);
					else
						fputs("   VALUES(", expstate->fp);
				}

				if (expstate->format == CLIPBOARD_FORMAT_INSERT)
				{
					if (expstate->colno > 0)
						fputs(", ", expstate->fp);
				}
				else
				{
					if (expstate->colno > 0)
					{
						fprintf(expstate->fp, ",\t\t -- %d. %s\n",
									expstate->colno, expstate->colnames[expstate->colno - 1]);
						fputs("          ", expstate->fp);
					}
				}

				field = trim_str(field, &size, expstate->force8bit);
				_field = quote_sql_literal(field,
										   &size,
										   expstate->force8bit,
										   expstate->empty_string_is_null);

				fwrite(_field, size, 1, expstate->fp);

				expstate->colno += 1;

				if (_field != field)
					free(_field);
			}
		}
	}

	/*
	 * Export in formatted text format
	 */
	else if (expstate->format == CLIPBOARD_FORMAT_TEXT)
	{
		errno = 0;

		if (typ != 'N')
		{
			if (typ == 'I' || typ == 'd')
			{
				/* Ignore items outer to selected range */
				if (expstate->xmin != -1 &&
					(xpos <= expstate->xmin ||
					 expstate->xmax <= xpos))
				return true;
			}

			fwrite(field, size, 1, expstate->fp);
		}
		else
			fputc('\n', expstate->fp);
	}

	/*
	 * Export in CSV or TSV format
	 */
	else if (DSV_FORMAT_TYPE(expstate->format))
	{
		if (typ == 'N' && !expstate->copy_line_extended)
		{
			errno = 0;
			fputc('\n', expstate->fp);
		}
		else
		{
			if (typ == 'd')
			{
				int saved_errno = 0;
				char   *_field;

				/* Ignore items outer to selected range */
				if (expstate->xmin != -1 &&
					(xpos <= expstate->xmin ||
					 expstate->xmax <= xpos))
				return true;

				field = trim_str(field, &size, expstate->force8bit);

				_field = csv_format(field, &size,
									expstate->force8bit,
									expstate->empty_string_is_null);


				if (expstate->copy_line_extended && is_colname)
				{

					if (!expstate->colnames)
					{
						expstate->colnames = smalloc(sizeof(char *) * expstate->columns);
						memset(expstate->colnames, 0, sizeof(char *) * expstate->columns);
					}

					if (!_field)
						expstate->colnames[expstate->colno] = sstrdup("");
					else if (_field != field)
						expstate->colnames[expstate->colno] = _field;
					else
						expstate->colnames[expstate->colno] = sstrndup(field, size);

					expstate->colno += 1;

					return true;
				}

				errno = 0;

				if (!expstate->copy_line_extended)
				{
					if (expstate->colno > 0)
					{

						if (expstate->format == CLIPBOARD_FORMAT_CSV)
							fputc(',', expstate->fp);
						else if (expstate->format == CLIPBOARD_FORMAT_TSVC)
							fputc('\t', expstate->fp);
					}

					if (_field && !errno)
					{
						errno = 0;
						fwrite(_field, size, 1, expstate->fp);
					}
				}
				else
					fprintf(expstate->fp, "%s,%.*s\n",
								  expstate->colnames[expstate->colno],
								  size, _field);

				saved_errno = errno;

				expstate->colno += 1;

				if (_field != field)
					free(_field);

				errno = saved_errno;
			}
		}
	}

	if (errno != 0)
	{
		format_error("%s", strerror(errno));
		log_row("Cannot write (%s)", current_state->errstr);

		return false;
	}

	return true;
}

/*
 * Exports data to defined stream in requested format.
 * Returns true, when the operation was successfull
 */
bool
export_data(Options *opts,
			ScrDesc *scrdesc,
			DataDesc *desc,
			int cursor_row,
			int cursor_column,
			FILE *fp,
			int rows,
			double percent,
			char *table_name,
			PspgCommand cmd,
			ClipboardFormat format)
{
	LineBufferIter	lbi;
	LineBufferMark	lbm;

	int		rn;
	char   *rowstr;
	bool	print_header = true;
	bool	print_footer = true;
	bool	print_border = true;
	bool	print_header_line = true;
	bool	save_column_names = false;
	bool	has_selection;

	int		min_row = desc->first_data_row;
	int		max_row = desc->last_row;

	bool	isok = true;
	ExportState expstate;

	expstate.format = format;
	expstate.fp = fp;
	expstate.force8bit = opts->force8bit;
	expstate.empty_string_is_null = opts->empty_string_is_null;
	expstate.xmin = -1;
	expstate.xmax = -1;
	expstate.table_name = NULL;
	expstate.colnames = NULL;
	expstate.columns = desc->columns;
	expstate.copy_line_extended = (cmd == cmd_CopyLineExtended);

	current_state->errstr = NULL;

	has_selection =
		((scrdesc->selected_first_row != -1 && scrdesc->selected_rows > 0 ) ||
		 (scrdesc->selected_first_column != -1 && scrdesc->selected_columns > 0));

	if (cmd == cmd_CopyLineExtended && !DSV_FORMAT_TYPE(format))
		format = CLIPBOARD_FORMAT_CSV;

	if (cmd == cmd_CopyLineExtended ||
		INSERT_FORMAT_TYPE(format))
	{
		if (INSERT_FORMAT_TYPE(format))
		{
			int		slen = strlen(table_name);

			expstate.table_name = quote_sql_identifier(table_name,
													   &slen,
													   opts->force8bit);
		}

		save_column_names = true;
	}

	if (cmd == cmd_CopyLine ||
		cmd == cmd_CopyLineExtended ||
		(cmd == cmd_Copy && !opts->no_cursor && !has_selection))
	{
		min_row = max_row = cursor_row + desc->first_data_row;
		print_footer = false;
	}

	if ((cmd == cmd_Copy && opts->vertical_cursor) ||
		cmd == cmd_CopyColumn)
	{
		expstate.xmin = desc->cranges[cursor_column - 1].xmin;
		expstate.xmax = desc->cranges[cursor_column - 1].xmax;

		print_footer = false;
	}

	/* copy value from cross of vertical and horizontal cursor */
	if (cmd == cmd_Copy && !opts->no_cursor && opts->vertical_cursor)
	{
		print_header = false;
		print_header_line = false;
		print_border = false;
	}

	if (cmd == cmd_CopyTopLines ||
		cmd == cmd_CopyBottomLines)
	{
		int		skip_data_rows;

		if (rows < 0 || percent < 0.0)
		{
			format_error("arguments (\"rows\" or \"percent\") of function export_data are negative");
			return false;
		}

		if (percent != 0.0)
			rows = (double) (desc->last_data_row - desc->first_data_row + 1) * (percent / 100.0);

		if (cmd == cmd_CopyBottomLines)
			skip_data_rows = desc->last_data_row - desc->first_data_row + 1 - rows;
		else
			skip_data_rows = 0;

		min_row += skip_data_rows;
		max_row = desc->first_data_row + rows - 1 + skip_data_rows;

		print_footer = false;
	}

	if (cmd == cmd_CopyMarkedLines || cmd == cmd_CopySearchedLines)
		print_footer = false;

	if ((cmd == cmd_Copy && has_selection) ||
		cmd == cmd_CopySelected)
	{
		if (scrdesc->selected_first_row != -1)
		{
			min_row = scrdesc->selected_first_row + desc->first_data_row;
			max_row = min_row + scrdesc->selected_rows - 1;
		}

		if (scrdesc->selected_first_column != -1 && scrdesc->selected_columns > 0)
		{
			expstate.xmin = scrdesc->selected_first_column;
			expstate.xmax = expstate.xmin + scrdesc->selected_columns - 1;
		}

		if (min_row > desc->first_data_row || max_row < desc->last_data_row)
			print_footer = false;
	}

	if (format != CLIPBOARD_FORMAT_TEXT)
	{
		print_border = false;
		print_footer = false;
		print_header_line = false;
	}

	if (save_column_names)
		print_header = true;

	init_lbi_ddesc(&lbi, desc, 0);

	while (lbi_set_mark_next(&lbi, &lbm))
	{
		LineInfo *linfo;

		int		size, width;
		int		field_size;
		int		field_xpos;
		int		colno;
		int		xpos;
		char   *field, *ptr, typ;
		FmtLineIter iter;
		bool	is_colname = false;

		(void) lbm_get_line(&lbm, &rowstr, &linfo, &rn);

		/* reduce rows from export */
		if (rn >= desc->first_data_row && rn <= desc->last_data_row)
		{
			if (rn < min_row || rn > max_row)
				continue;

			if (cmd == cmd_CopyMarkedLines)
			{
				if (!linfo || ((linfo->mask & LINEINFO_BOOKMARK) == 0))
					continue;
			}
			else if (cmd == cmd_CopyLine)
			{
				if (rn - desc->first_data_row != cursor_row)
					continue;
			}
			if (cmd == cmd_CopySearchedLines)
			{
				/* force lineinfo setting */
				linfo = set_line_info(opts, scrdesc, &lbm, rowstr);

				if (!linfo || ((linfo->mask & LINEINFO_FOUNDSTR) == 0))
					continue;
			}
		}
		else
		{
			is_colname = (rn != desc->border_top_row &&
						  rn != desc->border_bottom_row &&
						  rn != desc->border_head_row &&
						  rn <= desc->fixed_rows);

			if (!print_border &&
				(rn == desc->border_top_row ||
				 rn == desc->border_bottom_row))
				continue;
			if (!print_header_line &&
				rn == desc->border_head_row)
				continue;
			if (!print_header && rn < desc->fixed_rows)
				continue;
			if (!print_footer && desc->footer_row != -1 && rn >= desc->footer_row)
				continue;
		}

		iter.headline = desc->headline_transl;
		iter.row = rowstr;
		iter.force8bit = opts->force8bit;
		iter.xpos = 0;

		field = NULL; field_size = 0; field_xpos = -1;

		colno = 0;
		expstate.colno = 0;

		/*
		 * line parser - separates fields on line
		 */
		while ((ptr = next_char(&iter, &typ, &size, &width, &xpos)))
		{
			if (typ == 'd')
			{
				if (!field)
					field = ptr;

				field_size += size;
				field_xpos = xpos;
				continue;
			}

			if (field)
			{
				isok = process_item(&expstate, 'd',
									field, field_size, field_xpos,
									is_colname);
				if (!isok)
					goto exit_export;

				field = NULL; field_size = 0; field_xpos = -1;
			}

			isok = process_item(&expstate, typ,
								ptr, size, xpos,
								is_colname);

			if (!isok)
				goto exit_export;

			if (typ == 'I')
				colno += 1;
		}

		if (field)
		{
			isok = process_item(&expstate, 'd',
								field, field_size, field_xpos,
								is_colname);
			if (!isok)
				goto exit_export;
		}

		isok = process_item(&expstate, 'N',
							NULL, 0, -1, is_colname);
		if (!isok)
			goto exit_export;
	}

exit_export:

	if (expstate.colnames)
	{
		int		i;

		for (i = 0; i < expstate.columns; i++)
			free(expstate.colnames[i]);

		free(expstate.colnames);
	}

	if (expstate.table_name && expstate.table_name != table_name)
		free(expstate.table_name);

	return isok;
}
