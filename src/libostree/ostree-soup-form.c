/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*- */
/* soup-form.c : utility functions for HTML forms */

/*
 * Copyright 2008 Red Hat, Inc.
 */

/* This one is stripped down to only have soup_form_encode_hash()
 * and soup_form_encode_valist() which are the only bits that soup-uri.c
 * calls.
 */

#include <config.h>

#include <string.h>

#include "ostree-soup-uri.h"

/**
 * SECTION:soup-form
 * @short_description: HTML form handling
 * @see_also: #SoupMultipart
 *
 * libsoup contains several help methods for processing HTML forms as
 * defined by <ulink
 * url="http://www.w3.org/TR/html401/interact/forms.html#h-17.13">the
 * HTML 4.01 specification</ulink>.
 **/

/**
 * SOUP_FORM_MIME_TYPE_URLENCODED:
 *
 * A macro containing the value
 * <literal>"application/x-www-form-urlencoded"</literal>; the default
 * MIME type for POSTing HTML form data.
 *
 * Since: 2.26
 **/

/**
 * SOUP_FORM_MIME_TYPE_MULTIPART:
 *
 * A macro containing the value
 * <literal>"multipart/form-data"</literal>; the MIME type used for
 * posting form data that contains files to be uploaded.
 *
 * Since: 2.26
 **/

#define XDIGIT(c) ((c) <= '9' ? (c) - '0' : ((c) & 0x4F) - 'A' + 10)
#define HEXCHAR(s) ((XDIGIT (s[1]) << 4) + XDIGIT (s[2]))

static void
append_form_encoded (GString *str, const char *in)
{
	const unsigned char *s = (const unsigned char *)in;

	while (*s) {
		if (*s == ' ') {
			g_string_append_c (str, '+');
			s++;
		} else if (!g_ascii_isalnum (*s) && (*s != '-') && (*s != '_')
			   && (*s != '.'))
			g_string_append_printf (str, "%%%02X", (int)*s++);
		else
			g_string_append_c (str, *s++);
	}
}

static void
encode_pair (GString *str, const char *name, const char *value)
{
	g_return_if_fail (name != NULL);
	g_return_if_fail (value != NULL);

	if (str->len)
		g_string_append_c (str, '&');
	append_form_encoded (str, name);
	g_string_append_c (str, '=');
	append_form_encoded (str, value);
}

/**
 * soup_form_encode_hash:
 * @form_data_set: (element-type utf8 utf8): a hash table containing
 * name/value pairs (as strings)
 *
 * Encodes @form_data_set into a value of type
 * "application/x-www-form-urlencoded", as defined in the HTML 4.01
 * spec.
 *
 * Note that the HTML spec states that "The control names/values are
 * listed in the order they appear in the document." Since this method
 * takes a hash table, it cannot enforce that; if you care about the
 * ordering of the form fields, use soup_form_encode_datalist().
 *
 * Return value: the encoded form
 **/
char *
soup_form_encode_hash (GHashTable *form_data_set)
{
	GString *str = g_string_new (NULL);
	GHashTableIter iter;
	gpointer name, value;

	g_hash_table_iter_init (&iter, form_data_set);
	while (g_hash_table_iter_next (&iter, &name, &value))
		encode_pair (str, name, value);
	return g_string_free (str, FALSE);
}

/**
 * soup_form_encode_valist:
 * @first_field: name of the first form field
 * @args: pointer to additional values, as in soup_form_encode()
 *
 * See soup_form_encode(). This is mostly an internal method, used by
 * various other methods such as soup_uri_set_query_from_fields() and
 * soup_form_request_new().
 *
 * Return value: the encoded form
 **/
char *
soup_form_encode_valist (const char *first_field, va_list args)
{
	GString *str = g_string_new (NULL);
	const char *name, *value;

	name = first_field;
	value = va_arg (args, const char *);
	while (name && value) {
		encode_pair (str, name, value);

		name = va_arg (args, const char *);
		if (name)
			value = va_arg (args, const char *);
	}

	return g_string_free (str, FALSE);
}
