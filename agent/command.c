/* command.c - gpg-agent command handler
 * Copyright (C) 2001, 2002, 2003, 2004, 2005,
 *               2006  Free Software Foundation, Inc.
 *
 * This file is part of GnuPG.
 *
 * GnuPG is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * GnuPG is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, see <http://www.gnu.org/licenses/>.
 */

/* FIXME: we should not use the default assuan buffering but setup
   some buffering in secure mempory to protect session keys etc. */

#include <config.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <assert.h>

#include <assuan.h>

#include "agent.h"

/* maximum allowed size of the inquired ciphertext */
#define MAXLEN_CIPHERTEXT 4096
/* maximum allowed size of the key parameters */
#define MAXLEN_KEYPARAM 1024

#define set_error(e,t) assuan_set_error (ctx, gpg_error (e), (t))


#if MAX_DIGEST_LEN < 20
#error MAX_DIGEST_LEN shorter than keygrip
#endif

/* Data used to associate an Assuan context with local server data */
struct server_local_s
{
  assuan_context_t assuan_ctx;
  int message_fd;
  int use_cache_for_signing;
  char *keydesc;  /* Allocated description for the next key
                     operation. */
  int pause_io_logging; /* Used to suppress I/O logging during a command */
#ifdef HAVE_W32_SYSTEM
  int stopme;    /* If set to true the agent will be terminated after
                    the end of this session.  */
#endif
};


/* An entry for the getval/putval commands. */
struct putval_item_s
{
  struct putval_item_s *next;
  size_t off;  /* Offset to the value into DATA.  */
  size_t len;  /* Length of the value.  */
  char d[1];   /* Key | Nul | value.  */ 
};


/* A list of key value pairs fpr the getval/putval commands.  */
static struct putval_item_s *putval_list;



/* To help polling clients, we keep tarck of the number of certain
   events.  This structure keeps those counters.  The counters are
   integers and there should be no problem if they are overflowing as
   callers need to check only whether a counter changed.  The actual
   values are not meaningful. */
struct 
{
  /* Incremented if any of the other counters below changed. */
  unsigned int any;

  /* Incremented if a key is added or removed from the internal privat
     key database. */
  unsigned int key; 

  /* Incremented if a change of the card readers stati has been
     detected. */
  unsigned int card;

} eventcounter;





/* Release the memory buffer MB but first wipe out the used memory. */
static void
clear_outbuf (membuf_t *mb)
{
  void *p;
  size_t n;

  p = get_membuf (mb, &n);
  if (p)
    {
      memset (p, 0, n);
      xfree (p);
    }
}


/* Write the content of memory buffer MB as assuan data to CTX and
   wipe the buffer out afterwards. */
static gpg_error_t
write_and_clear_outbuf (assuan_context_t ctx, membuf_t *mb)
{
  assuan_error_t ae;
  void *p;
  size_t n;

  p = get_membuf (mb, &n);
  if (!p)
    return out_of_core ();
  ae = assuan_send_data (ctx, p, n);
  memset (p, 0, n);
  xfree (p);
  return ae;
}


static void
reset_notify (assuan_context_t ctx)
{
  ctrl_t ctrl = assuan_get_pointer (ctx);

  memset (ctrl->keygrip, 0, 20);
  ctrl->have_keygrip = 0;
  ctrl->digest.valuelen = 0;

  xfree (ctrl->server_local->keydesc);
  ctrl->server_local->keydesc = NULL;
}


/* Check whether the option NAME appears in LINE */
static int
has_option (const char *line, const char *name)
{
  const char *s;
  int n = strlen (name);

  s = strstr (line, name);
  return (s && (s == line || spacep (s-1)) && (!s[n] || spacep (s+n)));
}

/* Same as has_option but does only test for the name of the option
   and ignores an argument, i.e. with NAME being "--hash" it would
   return true for "--hash" as well as for "--hash=foo". */
static int
has_option_name (const char *line, const char *name)
{
  const char *s;
  int n = strlen (name);

  s = strstr (line, name);
  return (s && (s == line || spacep (s-1))
          && (!s[n] || spacep (s+n) || s[n] == '='));
}


/* Skip over options.  It is assumed that leading spaces have been
   removed (this is the case for lines passed to a handler from
   assuan).  Blanks after the options are also removed. */
static char *
skip_options (char *line)
{
  while ( *line == '-' && line[1] == '-' )
    {
      while (*line && !spacep (line))
        line++;
      while (spacep (line))
        line++;
    }
  return line;
}


/* Replace all '+' by a blank. */
static void
plus_to_blank (char *s)
{
  for (; *s; s++)
    {
      if (*s == '+')
        *s = ' ';
    }
}


/* Do the percent and plus/space unescaping in place and return the
   length of the valid buffer. */
static size_t
percent_plus_unescape (char *string)
{
  unsigned char *p = (unsigned char *)string;
  size_t n = 0;

  while (*string)
    {
      if (*string == '%' && string[1] && string[2])
        { 
          string++;
          *p++ = xtoi_2 (string);
          n++;
          string+= 2;
        }
      else if (*string == '+')
        {
          *p++ = ' ';
          n++;
          string++;
        }
      else
        {
          *p++ = *string++;
          n++;
        }
    }

  return n;
}




/* Parse a hex string.  Return an Assuan error code or 0 on success and the
   length of the parsed string in LEN. */
static int
parse_hexstring (assuan_context_t ctx, const char *string, size_t *len)
{
  const char *p;
  size_t n;

  /* parse the hash value */
  for (p=string, n=0; hexdigitp (p); p++, n++)
    ;
  if (*p != ' ' && *p != '\t' && *p)
    return set_error (GPG_ERR_ASS_PARAMETER, "invalid hexstring");
  if ((n&1))
    return set_error (GPG_ERR_ASS_PARAMETER, "odd number of digits");
  *len = n;
  return 0;
}

/* Parse the keygrip in STRING into the provided buffer BUF.  BUF must
   provide space for 20 bytes. BUF is not changed if the function
   returns an error. */
static int
parse_keygrip (assuan_context_t ctx, const char *string, unsigned char *buf)
{
  int rc;
  size_t n;
  const unsigned char *p;

  rc = parse_hexstring (ctx, string, &n);
  if (rc)
    return rc;
  n /= 2;
  if (n != 20)
    return set_error (GPG_ERR_ASS_PARAMETER, "invalid length of keygrip");

  for (p=(const unsigned char*)string, n=0; n < 20; p += 2, n++)
    buf[n] = xtoi_2 (p);

  return 0;
}


/* Write an assuan status line. */
gpg_error_t
agent_write_status (ctrl_t ctrl, const char *keyword, ...)
{
  gpg_error_t err = 0;
  va_list arg_ptr;
  const char *text;
  assuan_context_t ctx = ctrl->server_local->assuan_ctx;
  char buf[950], *p;
  size_t n;

  va_start (arg_ptr, keyword);

  p = buf; 
  n = 0;
  while ( (text = va_arg (arg_ptr, const char *)) )
    {
      if (n)
        {
          *p++ = ' ';
          n++;
        }
      for ( ; *text && n < DIM (buf)-2; n++)
        *p++ = *text++;
    }
  *p = 0;
  err = assuan_write_status (ctx, keyword, buf);

  va_end (arg_ptr);
  return err;
}



/* GETEVENTCOUNTER

   Return a a status line named EVENTCOUNTER with the current values
   of all event counters.  The values are decimal numbers in the range
   0 to UINT_MAX and wrapping around to 0.  The actual values should
   not be relied upon, they shall only be used to detect a change.

   The currently defined counters are:

   ANY  - Incremented with any change of any of the other counters.
   KEY  - Incremented for added or removed private keys.
   CARD - Incremented for changes of the card readers stati.
*/
static int
cmd_geteventcounter (assuan_context_t ctx, char *line)
{
  ctrl_t ctrl = assuan_get_pointer (ctx);
  char any_counter[25];
  char key_counter[25];
  char card_counter[25];

  snprintf (any_counter, sizeof any_counter, "%u", eventcounter.any);
  snprintf (key_counter, sizeof key_counter, "%u", eventcounter.key);
  snprintf (card_counter, sizeof card_counter, "%u", eventcounter.card);

  return agent_write_status (ctrl, "EVENTCOUNTER",
                             any_counter,
                             key_counter,
                             card_counter,
                             NULL);
}


/* This function should be called once for all key removals or
   additions.  Thus function is assured not to do any context
   switches. */
void
bump_key_eventcounter (void)
{
  eventcounter.key++;
  eventcounter.any++;
}

/* This function should be called for all card reader status
   changes. Thus function is assured not to do any context
   switches. */
void
bump_card_eventcounter (void)
{
  eventcounter.card++;
  eventcounter.any++;
}




/* ISTRUSTED <hexstring_with_fingerprint>

   Return OK when we have an entry with this fingerprint in our
   trustlist */
static int
cmd_istrusted (assuan_context_t ctx, char *line)
{
  ctrl_t ctrl = assuan_get_pointer (ctx);
  int rc, n, i;
  char *p;
  char fpr[41];

  /* Parse the fingerprint value. */
  for (p=line,n=0; hexdigitp (p); p++, n++)
    ;
  if (*p || !(n == 40 || n == 32))
    return set_error (GPG_ERR_ASS_PARAMETER, "invalid fingerprint");
  i = 0;
  if (n==32)
    {
      strcpy (fpr, "00000000");
      i += 8;
    }
  for (p=line; i < 40; p++, i++)
    fpr[i] = *p >= 'a'? (*p & 0xdf): *p;
  fpr[i] = 0;
  rc = agent_istrusted (ctrl, fpr);
  if (!rc || gpg_err_code (rc) == GPG_ERR_NOT_TRUSTED)
    return rc;
  else if (rc == -1 || gpg_err_code (rc) == GPG_ERR_EOF )
    return gpg_error (GPG_ERR_NOT_TRUSTED);
  else
    {
      log_error ("command is_trusted failed: %s\n", gpg_strerror (rc));
      return rc;
    }
}

/* LISTTRUSTED 

   List all entries from the trustlist */
static int
cmd_listtrusted (assuan_context_t ctx, char *line)
{
  int rc = agent_listtrusted (ctx);
  if (rc)
    log_error ("command listtrusted failed: %s\n", gpg_strerror (rc));
  return rc;
}


/* MARKTRUSTED <hexstring_with_fingerprint> <flag> <display_name>

   Store a new key in into the trustlist*/
static int
cmd_marktrusted (assuan_context_t ctx, char *line)
{
  ctrl_t ctrl = assuan_get_pointer (ctx);
  int rc, n, i;
  char *p;
  char fpr[41];
  int flag;

  /* parse the fingerprint value */
  for (p=line,n=0; hexdigitp (p); p++, n++)
    ;
  if (!spacep (p) || !(n == 40 || n == 32))
    return set_error (GPG_ERR_ASS_PARAMETER, "invalid fingerprint");
  i = 0;
  if (n==32)
    {
      strcpy (fpr, "00000000");
      i += 8;
    }
  for (p=line; i < 40; p++, i++)
    fpr[i] = *p >= 'a'? (*p & 0xdf): *p;
  fpr[i] = 0;
  
  while (spacep (p))
    p++;
  flag = *p++;
  if ( (flag != 'S' && flag != 'P') || !spacep (p) )
    return set_error (GPG_ERR_ASS_PARAMETER, "invalid flag - must be P or S");
  while (spacep (p))
    p++;

  rc = agent_marktrusted (ctrl, p, fpr, flag);
  if (rc)
    log_error ("command marktrusted failed: %s\n", gpg_strerror (rc));
  return rc;
}




/* HAVEKEY <hexstring_with_keygrip>
  
   Return success when the secret key is available */
static int
cmd_havekey (assuan_context_t ctx, char *line)
{
  int rc;
  unsigned char buf[20];

  rc = parse_keygrip (ctx, line, buf);
  if (rc)
    return rc;

  if (agent_key_available (buf))
    return gpg_error (GPG_ERR_NO_SECKEY);

  return 0;
}


/* SIGKEY <hexstring_with_keygrip>
   SETKEY <hexstring_with_keygrip>
  
   Set the  key used for a sign or decrypt operation */
static int
cmd_sigkey (assuan_context_t ctx, char *line)
{
  int rc;
  ctrl_t ctrl = assuan_get_pointer (ctx);

  rc = parse_keygrip (ctx, line, ctrl->keygrip);
  if (rc)
    return rc;
  ctrl->have_keygrip = 1;
  return 0;
}


/* SETKEYDESC plus_percent_escaped_string

   Set a description to be used for the next PKSIGN or PKDECRYPT
   operation if this operation requires the entry of a passphrase.  If
   this command is not used a default text will be used.  Note, that
   this description implictly selects the label used for the entry
   box; if the string contains the string PIN (which in general will
   not be translated), "PIN" is used, otherwise the translation of
   "passphrase" is used.  The description string should not contain
   blanks unless they are percent or '+' escaped.

   The description is only valid for the next PKSIGN or PKDECRYPT
   operation.
*/
static int
cmd_setkeydesc (assuan_context_t ctx, char *line)
{
  ctrl_t ctrl = assuan_get_pointer (ctx);
  char *desc, *p;

  for (p=line; *p == ' '; p++)
    ;
  desc = p;
  p = strchr (desc, ' ');
  if (p)
    *p = 0; /* We ignore any garbage; we might late use it for other args. */

  if (!desc || !*desc)
    return set_error (GPG_ERR_ASS_PARAMETER, "no description given");

  /* Note, that we only need to replace the + characters and should
     leave the other escaping in place because the escaped string is
     send verbatim to the pinentry which does the unescaping (but not
     the + replacing) */
  plus_to_blank (desc);

  xfree (ctrl->server_local->keydesc);
  ctrl->server_local->keydesc = xtrystrdup (desc);
  if (!ctrl->server_local->keydesc)
    return out_of_core ();
  return 0;
}


/* SETHASH --hash=<name>|<algonumber> <hexstring> 

  The client can use this command to tell the server about the data
  (which usually is a hash) to be signed. */
static int
cmd_sethash (assuan_context_t ctx, char *line)
{
  int rc;
  size_t n;
  char *p;
  ctrl_t ctrl = assuan_get_pointer (ctx);
  unsigned char *buf;
  char *endp;
  int algo;

  /* Parse the alternative hash options which may be used instead of
     the algo number.  */
  if (has_option_name (line, "--hash"))
    {
      if (has_option (line, "--hash=sha1"))
        algo = GCRY_MD_SHA1;
      else if (has_option (line, "--hash=sha256"))
        algo = GCRY_MD_SHA256;
      else if (has_option (line, "--hash=rmd160"))
        algo = GCRY_MD_RMD160;
      else if (has_option (line, "--hash=md5"))
        algo = GCRY_MD_MD5;
      else if (has_option (line, "--hash=tls-md5sha1"))
        algo = GCRY_MD_USER_TLS_MD5SHA1;
      else
        return set_error (GPG_ERR_ASS_PARAMETER, "invalid hash algorithm");
    }
  else
    algo = 0;

  line = skip_options (line);
  
  if (!algo)
    {
      /* No hash option has been given: require an algo number instead  */
      algo = (int)strtoul (line, &endp, 10);
      for (line = endp; *line == ' ' || *line == '\t'; line++)
        ;
      if (!algo || gcry_md_test_algo (algo))
        return set_error (GPG_ERR_UNSUPPORTED_ALGORITHM, NULL);
    }
  ctrl->digest.algo = algo;

  /* Parse the hash value. */
  rc = parse_hexstring (ctx, line, &n);
  if (rc)
    return rc;
  n /= 2;
  if (algo == GCRY_MD_USER_TLS_MD5SHA1 && n == 36)
    ;
  else if (n != 16 && n != 20 && n != 24 && n != 32)
    return set_error (GPG_ERR_ASS_PARAMETER, "unsupported length of hash");

  if (n > MAX_DIGEST_LEN)
    return set_error (GPG_ERR_ASS_PARAMETER, "hash value to long");

  buf = ctrl->digest.value;
  ctrl->digest.valuelen = n;
  for (p=line, n=0; n < ctrl->digest.valuelen; p += 2, n++)
    buf[n] = xtoi_2 (p);
  for (; n < ctrl->digest.valuelen; n++)
    buf[n] = 0;
  return 0;
}


/* PKSIGN <options>

   Perform the actual sign operation. Neither input nor output are
   sensitive to eavesdropping. */
static int
cmd_pksign (assuan_context_t ctx, char *line)
{
  int rc;
  cache_mode_t cache_mode = CACHE_MODE_NORMAL;
  ctrl_t ctrl = assuan_get_pointer (ctx);
  membuf_t outbuf;
  
  if (opt.ignore_cache_for_signing)
    cache_mode = CACHE_MODE_IGNORE;
  else if (!ctrl->server_local->use_cache_for_signing)
    cache_mode = CACHE_MODE_IGNORE;

  init_membuf (&outbuf, 512);

  rc = agent_pksign (ctrl, ctrl->server_local->keydesc,
                     &outbuf, cache_mode);
  if (rc)
    clear_outbuf (&outbuf);
  else
    rc = write_and_clear_outbuf (ctx, &outbuf);
  if (rc)
    log_error ("command pksign failed: %s\n", gpg_strerror (rc));
  xfree (ctrl->server_local->keydesc);
  ctrl->server_local->keydesc = NULL;
  return rc;
}

/* PKDECRYPT <options>

   Perform the actual decrypt operation.  Input is not 
   sensitive to eavesdropping */
static int
cmd_pkdecrypt (assuan_context_t ctx, char *line)
{
  int rc;
  ctrl_t ctrl = assuan_get_pointer (ctx);
  unsigned char *value;
  size_t valuelen;
  membuf_t outbuf;

  /* First inquire the data to decrypt */
  rc = assuan_inquire (ctx, "CIPHERTEXT",
                       &value, &valuelen, MAXLEN_CIPHERTEXT);
  if (rc)
    return rc;

  init_membuf (&outbuf, 512);

  rc = agent_pkdecrypt (ctrl, ctrl->server_local->keydesc,
                        value, valuelen, &outbuf);
  xfree (value);
  if (rc)
    clear_outbuf (&outbuf);
  else
    rc = write_and_clear_outbuf (ctx, &outbuf);
  if (rc)
    log_error ("command pkdecrypt failed: %s\n", gpg_strerror (rc));
  xfree (ctrl->server_local->keydesc);
  ctrl->server_local->keydesc = NULL;
  return rc;
}


/* GENKEY

   Generate a new key, store the secret part and return the public
   part.  Here is an example transaction:

   C: GENKEY
   S: INQUIRE KEYPARM
   C: D (genkey (rsa (nbits  1024)))
   C: END
   S: D (public-key
   S: D   (rsa (n 326487324683264) (e 10001)))
   S  OK key created
*/

static int
cmd_genkey (assuan_context_t ctx, char *line)
{
  ctrl_t ctrl = assuan_get_pointer (ctx);
  int rc;
  unsigned char *value;
  size_t valuelen;
  membuf_t outbuf;

  /* First inquire the parameters */
  rc = assuan_inquire (ctx, "KEYPARAM", &value, &valuelen, MAXLEN_KEYPARAM);
  if (rc)
    return rc;

  init_membuf (&outbuf, 512);

  rc = agent_genkey (ctrl, (char*)value, valuelen, &outbuf);
  xfree (value);
  if (rc)
    clear_outbuf (&outbuf);
  else
    rc = write_and_clear_outbuf (ctx, &outbuf);
  if (rc)
    log_error ("command genkey failed: %s\n", gpg_strerror (rc));
  return rc;
}




/* READKEY <hexstring_with_keygrip>
  
   Return the public key for the given keygrip.  */
static int
cmd_readkey (assuan_context_t ctx, char *line)
{
  ctrl_t ctrl = assuan_get_pointer (ctx);
  int rc;
  unsigned char grip[20];
  gcry_sexp_t s_pkey = NULL;

  rc = parse_keygrip (ctx, line, grip);
  if (rc)
    return rc; /* Return immediately as this is already an Assuan error code.*/

  rc = agent_public_key_from_file (ctrl, grip, &s_pkey);
  if (!rc)
    {
      size_t len;
      unsigned char *buf;

      len = gcry_sexp_sprint (s_pkey, GCRYSEXP_FMT_CANON, NULL, 0);
      assert (len);
      buf = xtrymalloc (len);
      if (!buf)
        rc = gpg_error_from_syserror ();
      else
        {
          len = gcry_sexp_sprint (s_pkey, GCRYSEXP_FMT_CANON, buf, len);
          assert (len);
          rc = assuan_send_data (ctx, buf, len);
          xfree (buf);
        }
      gcry_sexp_release (s_pkey);
    }

  if (rc)
    log_error ("command readkey failed: %s\n", gpg_strerror (rc));
  return rc;
}






static int
send_back_passphrase (assuan_context_t ctx, int via_data, const char *pw)
{
  size_t n;
  int rc;

  assuan_begin_confidential (ctx);
  n = strlen (pw);
  if (via_data)
    rc = assuan_send_data (ctx, pw, n);
  else
    {
      char *p = xtrymalloc_secure (n*2+1);
      if (!p)
        rc = gpg_error_from_syserror ();
      else
        {
          bin2hex (pw, n, p);
          rc = assuan_set_okay_line (ctx, p);
          xfree (p);
        }
    }
  return rc;
}


/* GET_PASSPHRASE [--data] [--check] <cache_id>
                  [<error_message> <prompt> <description>]

   This function is usually used to ask for a passphrase to be used
   for conventional encryption, but may also be used by programs which
   need specal handling of passphrases.  This command uses a syntax
   which helps clients to use the agent with minimum effort.  The
   agent either returns with an error or with a OK followed by the hex
   encoded passphrase.  Note that the length of the strings is
   implicitly limited by the maximum length of a command.

   If the option "--data" is used the passphrase is returned by usual
   data lines and not on the okay line.

   If the option "--check" is used the passphrase constraints checks as
   implemented by gpg-agent are applied.  A check is not done if the
   passphrase has been found in the cache.
*/

static int
cmd_get_passphrase (assuan_context_t ctx, char *line)
{
  ctrl_t ctrl = assuan_get_pointer (ctx);
  int rc;
  const char *pw;
  char *response;
  char *cacheid = NULL, *desc = NULL, *prompt = NULL, *errtext = NULL;
  char *p;
  void *cache_marker;
  int opt_data, opt_check;

  opt_data = has_option (line, "--data");
  opt_check = has_option (line, "--check");
  line = skip_options (line);

  cacheid = line;
  p = strchr (cacheid, ' ');
  if (p)
    {
      *p++ = 0;
      while (*p == ' ')
        p++;
      errtext = p;
      p = strchr (errtext, ' ');
      if (p)
        {
          *p++ = 0;
          while (*p == ' ')
            p++;
          prompt = p;
          p = strchr (prompt, ' ');
          if (p)
            {
              *p++ = 0;
              while (*p == ' ')
                p++;
              desc = p;
              p = strchr (desc, ' ');
              if (p)
                *p = 0; /* Ignore trailing garbage. */
            }
        }
    }
  if (!cacheid || !*cacheid || strlen (cacheid) > 50)
    return set_error (GPG_ERR_ASS_PARAMETER, "invalid length of cacheID");
  if (!desc)
    return set_error (GPG_ERR_ASS_PARAMETER, "no description given");

  if (!strcmp (cacheid, "X"))
    cacheid = NULL;
  if (!strcmp (errtext, "X"))
    errtext = NULL;
  if (!strcmp (prompt, "X"))
    prompt = NULL;
  if (!strcmp (desc, "X"))
    desc = NULL;

  pw = cacheid ? agent_get_cache (cacheid, CACHE_MODE_NORMAL, &cache_marker)
               : NULL;
  if (pw)
    {
      rc = send_back_passphrase (ctx, opt_data, pw);
      agent_unlock_cache_entry (&cache_marker);
    }
  else
    {
      /* Note, that we only need to replace the + characters and
         should leave the other escaping in place because the escaped
         string is send verbatim to the pinentry which does the
         unescaping (but not the + replacing) */
      if (errtext)
        plus_to_blank (errtext);
      if (prompt)
        plus_to_blank (prompt);
      if (desc)
        plus_to_blank (desc);

      response = NULL;
      do
        {
          xfree (response);
          rc = agent_get_passphrase (ctrl, &response, desc, prompt, errtext);
        }
      while (!rc
             && opt_check
             && check_passphrase_constraints (ctrl, response));

      if (!rc)
        {
          if (cacheid)
            agent_put_cache (cacheid, CACHE_MODE_USER, response, 0);
          rc = send_back_passphrase (ctx, opt_data, response);
          xfree (response);
        }
    }

  if (rc)
    log_error ("command get_passphrase failed: %s\n", gpg_strerror (rc));
  return rc;
}


/* CLEAR_PASSPHRASE <cache_id>

   may be used to invalidate the cache entry for a passphrase.  The
   function returns with OK even when there is no cached passphrase.
*/

static int
cmd_clear_passphrase (assuan_context_t ctx, char *line)
{
  char *cacheid = NULL;
  char *p;

  /* parse the stuff */
  for (p=line; *p == ' '; p++)
    ;
  cacheid = p;
  p = strchr (cacheid, ' ');
  if (p)
    *p = 0; /* ignore garbage */
  if (!cacheid || !*cacheid || strlen (cacheid) > 50)
    return set_error (GPG_ERR_ASS_PARAMETER, "invalid length of cacheID");

  agent_put_cache (cacheid, CACHE_MODE_USER, NULL, 0);
  return 0;
}


/* GET_CONFIRMATION <description>

   This command may be used to ask for a simple confirmation.
   DESCRIPTION is displayed along with a Okay and Cancel button.  This
   command uses a syntax which helps clients to use the agent with
   minimum effort.  The agent either returns with an error or with a
   OK.  Note, that the length of DESCRIPTION is implicitly limited by
   the maximum length of a command. DESCRIPTION should not contain
   any spaces, those must be encoded either percent escaped or simply
   as '+'.
*/

static int
cmd_get_confirmation (assuan_context_t ctx, char *line)
{
  ctrl_t ctrl = assuan_get_pointer (ctx);
  int rc;
  char *desc = NULL;
  char *p;

  /* parse the stuff */
  for (p=line; *p == ' '; p++)
    ;
  desc = p;
  p = strchr (desc, ' ');
  if (p)
    *p = 0; /* We ignore any garbage -may be later used for other args. */

  if (!desc || !*desc)
    return set_error (GPG_ERR_ASS_PARAMETER, "no description given");

  if (!strcmp (desc, "X"))
    desc = NULL;

  /* Note, that we only need to replace the + characters and should
     leave the other escaping in place because the escaped string is
     send verbatim to the pinentry which does the unescaping (but not
     the + replacing) */
  if (desc)
    plus_to_blank (desc);

  rc = agent_get_confirmation (ctrl, desc, NULL, NULL);
  if (rc)
    log_error ("command get_confirmation failed: %s\n", gpg_strerror (rc));
  return rc;
}



/* LEARN [--send]

   Learn something about the currently inserted smartcard.  With
   --send the new certificates are send back.  */
static int
cmd_learn (assuan_context_t ctx, char *line)
{
  ctrl_t ctrl = assuan_get_pointer (ctx);
  int rc;

  rc = agent_handle_learn (ctrl, has_option (line, "--send")? ctx : NULL);
  if (rc)
    log_error ("command learn failed: %s\n", gpg_strerror (rc));
  return rc;
}



/* PASSWD <hexstring_with_keygrip>
  
   Change the passphrase/PID for the key identified by keygrip in LINE. */
static int
cmd_passwd (assuan_context_t ctx, char *line)
{
  ctrl_t ctrl = assuan_get_pointer (ctx);
  int rc;
  unsigned char grip[20];
  gcry_sexp_t s_skey = NULL;
  unsigned char *shadow_info = NULL;

  rc = parse_keygrip (ctx, line, grip);
  if (rc)
    goto leave;

  ctrl->in_passwd++;
  rc = agent_key_from_file (ctrl, ctrl->server_local->keydesc,
                            grip, &shadow_info, CACHE_MODE_IGNORE, &s_skey);
  if (rc)
    ;
  else if (!s_skey)
    {
      log_error ("changing a smartcard PIN is not yet supported\n");
      rc = gpg_error (GPG_ERR_NOT_IMPLEMENTED);
    }
  else
    rc = agent_protect_and_store (ctrl, s_skey);
  ctrl->in_passwd--;

  xfree (ctrl->server_local->keydesc);
  ctrl->server_local->keydesc = NULL;

 leave:
  gcry_sexp_release (s_skey);
  xfree (shadow_info);
  if (rc)
    log_error ("command passwd failed: %s\n", gpg_strerror (rc));
  return rc;
}

/* PRESET_PASSPHRASE <hexstring_with_keygrip> <timeout> <hexstring>
  
   Set the cached passphrase/PIN for the key identified by the keygrip
   to passwd for the given time, where -1 means infinite and 0 means
   the default (currently only a timeout of -1 is allowed, which means
   to never expire it).  If passwd is not provided, ask for it via the
   pinentry module.  */
static int
cmd_preset_passphrase (assuan_context_t ctx, char *line)
{
  int rc;
  unsigned char grip[20];
  char *grip_clear = NULL;
  char *passphrase = NULL;
  int ttl;
  size_t len;

  if (!opt.allow_preset_passphrase)
    return gpg_error (GPG_ERR_NOT_SUPPORTED);

  rc = parse_keygrip (ctx, line, grip);
  if (rc)
    return rc;

  /* FIXME: parse_keygrip should return a tail pointer.  */
  grip_clear = line;
  while (*line && (*line != ' ' && *line != '\t'))
    line++;
  if (!*line)
    return gpg_error (GPG_ERR_MISSING_VALUE);
  *line = '\0';
  line++;
  while (*line && (*line == ' ' || *line == '\t'))
    line++;
  
  /* Currently, only infinite timeouts are allowed.  */
  ttl = -1;
  if (line[0] != '-' || line[1] != '1')
    return gpg_error (GPG_ERR_NOT_IMPLEMENTED);
  line++;
  line++;
  while (!(*line != ' ' && *line != '\t'))
    line++;

  /* Syntax check the hexstring.  */
  rc = parse_hexstring (ctx, line, &len);
  if (rc)
    return rc;
  line[len] = '\0';

  /* If there is a passphrase, use it.  Currently, a passphrase is
     required.  */
  if (*line)
    passphrase = line;
  else
    return gpg_error (GPG_ERR_NOT_IMPLEMENTED);

  rc = agent_put_cache (grip_clear, CACHE_MODE_ANY, passphrase, ttl);

  if (rc)
    log_error ("command preset_passwd failed: %s\n", gpg_strerror (rc));

  return rc;
}


/* SCD <commands to pass to the scdaemon>
  
   This is a general quote command to redirect everything to the
   SCDAEMON. */
static int
cmd_scd (assuan_context_t ctx, char *line)
{
  ctrl_t ctrl = assuan_get_pointer (ctx);
  int rc;

  rc = divert_generic_cmd (ctrl, line, ctx);

  return rc;
}



/* GETVAL <key>

   Return the value for KEY from the special environment as created by
   PUTVAL.
 */
static int
cmd_getval (assuan_context_t ctx, char *line)
{
  int rc = 0;
  char *key = NULL;
  char *p;
  struct putval_item_s *vl;

  for (p=line; *p == ' '; p++)
    ;
  key = p;
  p = strchr (key, ' ');
  if (p)
    {
      *p++ = 0; 
      for (; *p == ' '; p++)
        ;
      if (*p)
        return set_error (GPG_ERR_ASS_PARAMETER, "too many arguments");
    }
  if (!key || !*key)
    return set_error (GPG_ERR_ASS_PARAMETER, "no key given");


  for (vl=putval_list; vl; vl = vl->next)
    if ( !strcmp (vl->d, key) )
      break;

  if (vl) /* Got an entry. */
    rc = assuan_send_data (ctx, vl->d+vl->off, vl->len);
  else
    return gpg_error (GPG_ERR_NO_DATA);

  if (rc)
    log_error ("command getval failed: %s\n", gpg_strerror (rc));
  return rc;
}


/* PUTVAL <key> [<percent_escaped_value>]

   The gpg-agent maintains a kind of environment which may be used to
   store key/value pairs in it, so that they can be retrieved later.
   This may be used by helper daemons to daemonize themself on
   invocation and register them with gpg-agent.  Callers of the
   daemon's service may now first try connect to get the information
   for that service from gpg-agent through the GETVAL command and then
   try to connect to that daemon.  Only if that fails they may start
   an own instance of the service daemon. 

   KEY is an an arbitrary symbol with the same syntax rules as keys
   for shell environment variables.  PERCENT_ESCAPED_VALUE is the
   corresponsing value; they should be similar to the values of
   envronment variables but gpg-agent does not enforce any
   restrictions.  If that value is not given any value under that KEY
   is removed from this special environment.
*/
static int
cmd_putval (assuan_context_t ctx, char *line)
{
  int rc = 0;
  char *key = NULL;
  char *value = NULL;
  size_t valuelen = 0;
  char *p;
  struct putval_item_s *vl, *vlprev;

  for (p=line; *p == ' '; p++)
    ;
  key = p;
  p = strchr (key, ' ');
  if (p)
    {
      *p++ = 0; 
      for (; *p == ' '; p++)
        ;
      if (*p)
        {
          value = p;
          p = strchr (value, ' ');
          if (p)
            *p = 0;
          valuelen = percent_plus_unescape (value);
        }
    }
  if (!key || !*key)
    return set_error (GPG_ERR_ASS_PARAMETER, "no key given");


  for (vl=putval_list,vlprev=NULL; vl; vlprev=vl, vl = vl->next)
    if ( !strcmp (vl->d, key) )
      break;

  if (vl) /* Delete old entry. */
    {
      if (vlprev)
        vlprev->next = vl->next;
      else
        putval_list = vl->next;
      xfree (vl);
    }

  if (valuelen) /* Add entry. */  
    {
      vl = xtrymalloc (sizeof *vl + strlen (key) + valuelen);
      if (!vl)
        rc = gpg_error_from_syserror ();
      else
        {
          vl->len = valuelen;
          vl->off = strlen (key) + 1;
          strcpy (vl->d, key);
          memcpy (vl->d + vl->off, value, valuelen);
          vl->next = putval_list;
          putval_list = vl;
        }
    }

  if (rc)
    log_error ("command putval failed: %s\n", gpg_strerror (rc));
  return rc;
}




/* UPDATESTARTUPTTY 
  
  Set startup TTY and X DISPLAY variables to the values of this
  session.  This command is useful to pull future pinentries to
  another screen.  It is only required because there is no way in the
  ssh-agent protocol to convey this information.  */
static int
cmd_updatestartuptty (assuan_context_t ctx, char *line)
{
  ctrl_t ctrl = assuan_get_pointer (ctx);

  xfree (opt.startup_display); opt.startup_display = NULL;
  xfree (opt.startup_ttyname); opt.startup_ttyname = NULL;
  xfree (opt.startup_ttytype); opt.startup_ttytype = NULL;
  xfree (opt.startup_lc_ctype); opt.startup_lc_ctype = NULL;
  xfree (opt.startup_lc_messages); opt.startup_lc_messages = NULL;

  if (ctrl->display)
    opt.startup_display = xtrystrdup (ctrl->display);
  if (ctrl->ttyname)
    opt.startup_ttyname = xtrystrdup (ctrl->ttyname);
  if (ctrl->ttytype)
    opt.startup_ttytype = xtrystrdup (ctrl->ttytype);
  if (ctrl->lc_ctype) 
    opt.startup_lc_ctype = xtrystrdup (ctrl->lc_ctype);
  if (ctrl->lc_messages)
    opt.startup_lc_messages = xtrystrdup (ctrl->lc_messages);

  return 0;
}



#ifdef HAVE_W32_SYSTEM
/* KILLAGENT

   Under Windows we start the agent on the fly.  Thus it also make
   sense to allow a client to stop the agent. */
static int
cmd_killagent (assuan_context_t ctx, char *line)
{
  ctrl_t ctrl = assuan_get_pointer (ctx);
  ctrl->server_local->stopme = 1;
  return 0;
}

/* RELOADAGENT

   As signals are inconvenient under Windows, we provide this command
   to allow reloading of the configuration.  */
static int
cmd_reloadagent (assuan_context_t ctx, char *line)
{
  agent_sighup_action ();
  return 0;
}
#endif /*HAVE_W32_SYSTEM*/



/* GETINFO <what>

   Multipurpose function to return a variety of information.
   Supported values for WHAT are:

     version     - Return the version of the program.
     socket_name - Return the name of the socket.
     ssh_socket_name - Return the name of the ssh socket.

 */
static int
cmd_getinfo (assuan_context_t ctx, char *line)
{
  int rc = 0;

  if (!strcmp (line, "version"))
    {
      const char *s = VERSION;
      rc = assuan_send_data (ctx, s, strlen (s));
    }
  else if (!strcmp (line, "socket_name"))
    {
      const char *s = get_agent_socket_name ();

      if (s)
        rc = assuan_send_data (ctx, s, strlen (s));
      else
        rc = gpg_error (GPG_ERR_NO_DATA);
    }
  else if (!strcmp (line, "ssh_socket_name"))
    {
      const char *s = get_agent_ssh_socket_name ();

      if (s)
        rc = assuan_send_data (ctx, s, strlen (s));
      else
        rc = gpg_error (GPG_ERR_NO_DATA);
    }
  else
    rc = set_error (GPG_ERR_ASS_PARAMETER, "unknown value for WHAT");
  return rc;
}



static int
option_handler (assuan_context_t ctx, const char *key, const char *value)
{
  ctrl_t ctrl = assuan_get_pointer (ctx);

  if (!strcmp (key, "display"))
    {
      if (ctrl->display)
        free (ctrl->display);
      ctrl->display = strdup (value);
      if (!ctrl->display)
        return out_of_core ();
    }
  else if (!strcmp (key, "ttyname"))
    {
      if (!opt.keep_tty)
        {
          if (ctrl->ttyname)
            free (ctrl->ttyname);
          ctrl->ttyname = strdup (value);
          if (!ctrl->ttyname)
            return out_of_core ();
        }
    }
  else if (!strcmp (key, "ttytype"))
    {
      if (!opt.keep_tty)
        {
          if (ctrl->ttytype)
            free (ctrl->ttytype);
          ctrl->ttytype = strdup (value);
          if (!ctrl->ttytype)
            return out_of_core ();
        }
    }
  else if (!strcmp (key, "lc-ctype"))
    {
      if (ctrl->lc_ctype)
        free (ctrl->lc_ctype);
      ctrl->lc_ctype = strdup (value);
      if (!ctrl->lc_ctype)
        return out_of_core ();
    }
  else if (!strcmp (key, "lc-messages"))
    {
      if (ctrl->lc_messages)
        free (ctrl->lc_messages);
      ctrl->lc_messages = strdup (value);
      if (!ctrl->lc_messages)
        return out_of_core ();
    }
  else if (!strcmp (key, "use-cache-for-signing"))
    ctrl->server_local->use_cache_for_signing = *value? atoi (value) : 0;
  else
    return gpg_error (GPG_ERR_UNKNOWN_OPTION);

  return 0;
}




/* Called by libassuan after all commands. ERR is the error from the
   last assuan operation and not the one returned from the command. */
static void
post_cmd_notify (assuan_context_t ctx, int err)
{
  ctrl_t ctrl = assuan_get_pointer (ctx);

  /* Switch off any I/O monitor controlled logging pausing. */
  ctrl->server_local->pause_io_logging = 0;
}


/* This function is called by libassuan for all I/O.  We use it here
   to disable logging for the GETEVENTCOUNTER commands.  This is so
   that the debug output won't get cluttered by this primitive
   command.  */
static unsigned int
io_monitor (assuan_context_t ctx, int direction,
            const char *line, size_t linelen)
{
  ctrl_t ctrl = assuan_get_pointer (ctx);

  /* Note that we only check for the uppercase name.  This allows to
     see the logging for debugging if using a non-upercase command
     name. */
  if (ctx && !direction 
      && linelen >= 15
      && !strncmp (line, "GETEVENTCOUNTER", 15)
      && (linelen == 15 || spacep (line+15)))
    {
      ctrl->server_local->pause_io_logging = 1;
    }

  return ctrl->server_local->pause_io_logging? 1:0;
}


/* Tell the assuan library about our commands */
static int
register_commands (assuan_context_t ctx)
{
  static struct {
    const char *name;
    int (*handler)(assuan_context_t, char *line);
  } table[] = {
    { "GETEVENTCOUNTER",cmd_geteventcounter },
    { "ISTRUSTED",      cmd_istrusted },
    { "HAVEKEY",        cmd_havekey },
    { "SIGKEY",         cmd_sigkey },
    { "SETKEY",         cmd_sigkey },
    { "SETKEYDESC",     cmd_setkeydesc },
    { "SETHASH",        cmd_sethash },
    { "PKSIGN",         cmd_pksign },
    { "PKDECRYPT",      cmd_pkdecrypt },
    { "GENKEY",         cmd_genkey },
    { "READKEY",        cmd_readkey },
    { "GET_PASSPHRASE", cmd_get_passphrase },
    { "PRESET_PASSPHRASE", cmd_preset_passphrase },
    { "CLEAR_PASSPHRASE", cmd_clear_passphrase },
    { "GET_CONFIRMATION", cmd_get_confirmation },
    { "LISTTRUSTED",    cmd_listtrusted },
    { "MARKTRUSTED",    cmd_marktrusted },
    { "LEARN",          cmd_learn },
    { "PASSWD",         cmd_passwd },
    { "INPUT",          NULL }, 
    { "OUTPUT",         NULL }, 
    { "SCD",            cmd_scd },
    { "GETVAL",         cmd_getval },
    { "PUTVAL",         cmd_putval },
    { "UPDATESTARTUPTTY",  cmd_updatestartuptty },
#ifdef HAVE_W32_SYSTEM
    { "KILLAGENT",      cmd_killagent },
    { "RELOADAGENT",    cmd_reloadagent },
#endif
    { "GETINFO",        cmd_getinfo },
    { NULL }
  };
  int i, rc;

  for (i=0; table[i].name; i++)
    {
      rc = assuan_register_command (ctx, table[i].name, table[i].handler);
      if (rc)
        return rc;
    } 
#ifdef HAVE_ASSUAN_SET_IO_MONITOR
  assuan_register_post_cmd_notify (ctx, post_cmd_notify);
#endif
  assuan_register_reset_notify (ctx, reset_notify);
  assuan_register_option_handler (ctx, option_handler);
  return 0;
}


/* Startup the server.  If LISTEN_FD and FD is given as -1, this is a
   simple piper server, otherwise it is a regular server.  CTRL is the
   control structure for this connection; it has only the basic
   intialization. */
void
start_command_handler (ctrl_t ctrl, int listen_fd, int fd)
{
  int rc;
  assuan_context_t ctx;

  if (listen_fd == -1 && fd == -1)
    {
      int filedes[2];

      filedes[0] = 0;
      filedes[1] = 1;
      rc = assuan_init_pipe_server (&ctx, filedes);
    }
  else if (listen_fd != -1)
    {
      rc = assuan_init_socket_server_ext (&ctx, listen_fd, 0);
    }
  else 
    {
      rc = assuan_init_socket_server_ext (&ctx, fd, 2);
      ctrl->connection_fd = fd;
    }
  if (rc)
    {
      log_error ("failed to initialize the server: %s\n",
                 gpg_strerror(rc));
      agent_exit (2);
    }
  rc = register_commands (ctx);
  if (rc)
    {
      log_error ("failed to register commands with Assuan: %s\n",
                 gpg_strerror(rc));
      agent_exit (2);
    }

  assuan_set_pointer (ctx, ctrl);
  ctrl->server_local = xcalloc (1, sizeof *ctrl->server_local);
  ctrl->server_local->assuan_ctx = ctx;
  ctrl->server_local->message_fd = -1;
  ctrl->server_local->use_cache_for_signing = 1;
  ctrl->digest.raw_value = 0;

  if (DBG_ASSUAN)
    assuan_set_log_stream (ctx, log_get_stream ());

#ifdef HAVE_ASSUAN_SET_IO_MONITOR
  assuan_set_io_monitor (ctx, io_monitor);
#endif

  for (;;)
    {
      rc = assuan_accept (ctx);
      if (rc == -1)
        {
          break;
        }
      else if (rc)
        {
          log_info ("Assuan accept problem: %s\n", gpg_strerror (rc));
          break;
        }
      
      rc = assuan_process (ctx);
      if (rc)
        {
          log_info ("Assuan processing failed: %s\n", gpg_strerror (rc));
          continue;
        }
    }

  /* Reset the SCD if needed. */
  agent_reset_scd (ctrl);

  /* Reset the pinentry (in case of popup messages). */
  agent_reset_query (ctrl);

  /* Cleanup.  */
  assuan_deinit_server (ctx);
#ifdef HAVE_W32_SYSTEM
  if (ctrl->server_local->stopme)
    agent_exit (0);
#endif
  xfree (ctrl->server_local);
  ctrl->server_local = NULL;
}

