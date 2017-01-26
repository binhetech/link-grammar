/*************************************************************************/
/* Copyright (c) 2004                                                    */
/* Daniel Sleator, David Temperley, and John Lafferty                    */
/* Copyright 2008, 2009, 2012 Linas Vepstas <linasvepstas@gmail.com>     */
/* All rights reserved                                                   */
/*                                                                       */
/* Use of the link grammar parsing system is subject to the terms of the */
/* license set forth in the LICENSE file included with this software.    */
/* This license allows free redistribution and use in source and binary  */
/* forms, with or without modification, subject to certain conditions.   */
/*                                                                       */
/*************************************************************************/

#include "anysplit.h"
#include "api-structures.h"
#include "dict-api.h"
#include "dict-common.h"
#include "externs.h"
#include "idiom.h"
#include "pp_knowledge.h"
#include "read-dict.h"
#include "read-regex.h"
#include "regex-morph.h"
#include "spellcheck.h"
#include "string-set.h"
#include "structures.h"
#include "utilities.h"
#include "word-utils.h"
#include "dict-sql/read-sql.h"  /* Temporary hack */


/***************************************************************
*
* Routines for manipulating Dictionary
*
****************************************************************/

/* Units will typically have a ".u" at the end. Get
 * rid of it, as otherwise stripping is messed up. */
static inline char * deinflect(const char * str)
{
	size_t len;
	char *s;
	char *p = strrchr(str, SUBSCRIPT_MARK);
	if (!p || (p == str)) return strdup(str);

	len = p - str;
	s = (char *)malloc(len + 1);
	strncpy(s, str, len);
	s[len] = '\0';
	return s;
}

/* The affix dictionary is represented as a dynamically allocated array with
 * an element for each class (connector type) in the affix file. Each element
 * has a pointer to an array of strings which are the punctuation/affix
 * names. */

const char * afdict_classname[] = { AFDICT_CLASSNAMES };

/**
 * Find the affix table entry for given connector name.
 * If the connector name is not in the table, return NULL.
 */
Afdict_class * afdict_find(Dictionary afdict, const char * con, bool notify_err)
{
	const char ** ac;

	for (ac = afdict_classname;
	     ac < &afdict_classname[ARRAY_SIZE(afdict_classname)]; ac++)
	{
		if (0 == strcmp(*ac, con))
			return &afdict->afdict_class[ac - afdict_classname];
	}
	if (notify_err) {
		prt_error("Warning: Unknown class name %s found near line %d of %s.\n"
		          "\tThis class name will be ignored.\n",
		          con, afdict->line_number, afdict->name);
	}
	return NULL;
}

#define AFFIX_COUNT_MEM_INCREMENT 64

static void affix_list_add(Dictionary afdict, Afdict_class * ac,
		const char * affix)
{
	if (NULL == ac)  return; /* ignore unknown class name */
	if (ac->mem_elems <= ac->length)
	{
		size_t new_sz;
		ac->mem_elems += AFFIX_COUNT_MEM_INCREMENT;
		new_sz = ac->mem_elems * sizeof(const char *);
		ac->string = (char const **) realloc((void *)ac->string, new_sz);
	}
	ac->string[ac->length] = string_set_add(affix, afdict->string_set);
	ac->length++;
}

static void load_affix(Dictionary afdict, Dict_node *dn, int l)
{
	Dict_node * dnx = NULL;
	for (; NULL != dn; dn = dnx)
	{
		char *string;
		const char *con = word_only_connector(dn);
		if (NULL == con)
		{
			/* ??? should we support here more than one class? */
			prt_error("Warning: Word \"%s\" found near line %d of %s.\n"
			          "\tWord has more than one connector.\n"
			          "\tThis word will be ignored.\n",
			          dn->string, afdict->line_number, afdict->name);
			return;
		}

		/* The affix files serve a dual purpose: they indicate both
		 * what a unit is, connector-wise, and what is strippable, as
		 * a string.  When the unit is an 'idiom' (i.e. two words,
		 * e.g. base_pair or degrees_C) then only the first word can
		 * be stripped away from a run-on expression (e.g. "86degrees C")
		 */
		if (contains_underbar(dn->string))
		{
			char *p;
			string = strdup(dn->string);
			p = string+1;
			while (*p != '_' && *p != '\0') p++;
			*p = '\0';
		}
		else
		{
			string = deinflect(dn->string);
		}

		affix_list_add(afdict, afdict_find(afdict, con,
		               /*notify_err*/true), string);
		free(string);

		dnx = dn->left;
		xfree((char *)dn, sizeof(Dict_node));
	}
}

#ifdef AFDICT_ORDER_NOT_PRESERVED
static int revcmplen(const void *a, const void *b)
{
	return strlen(*(char * const *)b) - strlen(*(char * const *)a);
}
#endif /* AFDICT_ORDER_NOT_PRESERVED */

/**
 * Traverse the main dict in dictionary order, and extract all the suffixes
 * and prefixes - every time we see a new suffix/prefix (the previous one is
 * remembered by w_last), we save it in the corresponding affix-class list.
 * The saved affixes don't include the infix mark.
 */
static void get_dict_affixes(Dictionary dict, Dict_node * dn,
                             char infix_mark, char * w_last)
{
	const char *w;         /* current dict word */
	const char *w_sm;      /* SUBSCRIPT_MARK position in the dict word */
	size_t w_len;          /* length of the dict word */
	Dictionary afdict = dict->affix_table;

	if (dn == NULL) return;
	get_dict_affixes(dict, dn->right, infix_mark, w_last);

	w = dn->string;
	w_sm = strrchr(w, SUBSCRIPT_MARK);
	w_len = (NULL == w_sm) ? strlen(w) : (size_t)(w_sm - w);
	if (w_len > MAX_WORD)
	{
		prt_error("Error: word '%s' too long (%zd), program may malfunction\n",
		          w, w_len);
		w_len = MAX_WORD;
	}
	/* (strlen(w_last) can be cached for speedup) */
	if ((strlen(w_last) != w_len) || (0 != strncmp(w_last, w, w_len)))
	{
		strncpy(w_last, w, w_len);
		w_last[w_len] = '\0';

		if (infix_mark == w_last[0])
		{
			affix_list_add(afdict, &afdict->afdict_class[AFDICT_SUF], w_last+1);
		}
		else
		if (infix_mark == w_last[w_len-1])
		{
			w_last[w_len-1] = '\0';
			affix_list_add(afdict, &afdict->afdict_class[AFDICT_PRE], w_last);
			w_last[w_len-1] = infix_mark;
		}
	}

	get_dict_affixes(dict, dn->left, infix_mark, w_last);
}

/**
 * Concatenate the definitions for the given affix class.
 * This allows specifying the characters in different definitions
 * instead in a one long string, e.g. instead of:
 * ""«»《》【】『』`„": QUOTES+;
 * One can specify (note the added spaces):
 * """  «»  《》 【】 『』  ` „: QUOTES+;
 * Or even:
 * """: QUOTES+;
 * «» : QUOTES+;
 * etc.
 * Note that if there are no definitions or only one definition, there is
 * nothing to do.
 * The result is written to the first entry.
 * @param classno The given affix class.
 */
static void concat_class(Dictionary afdict, int classno)
{
	Afdict_class * ac;
	size_t i;
	dyn_str * qs;

	ac = AFCLASS(afdict, classno);
	if (1 >= ac->length) return;

	qs = dyn_str_new();
	for (i = 0; i < ac->length; i++)
		dyn_strcat(qs, ac->string[i]);

	ac->string[0] = string_set_add(qs->str, afdict->string_set);
	dyn_str_delete(qs);
}

/* Compare lengths of strings, for qsort */
static int cmplen(const void *a, const void *b)
{
	const char * const *sa = a;
	const char * const *sb = b;
	return strlen(*sb) - strlen(*sa);
}

/**
 * Initialize several classes.
 * In case of a future dynamic change of the affix table, this function needs to
 * be invoked again after the affix table is re-constructed (changes may be
 * needed - especially to first free memory and initialize the affix dict
 * structure.).
 */
#define D_AI 11
static bool afdict_init(Dictionary dict)
{
	Afdict_class * ac;
	Dictionary afdict = dict->affix_table;

	/* FIXME: read_entry() builds word lists in reverse order (can we
	 * just create the list top-down without breaking anything?). Unless
	 * it is fixed to preserve the order, reverse here the word list for
	 * each affix class. */
	for (ac = afdict->afdict_class;
		  ac < &afdict->afdict_class[ARRAY_SIZE(afdict_classname)]; ac++)
	{
		int i;
		int l = ac->length - 1;
		const char * t;

		for (i = 0;  i < l; i++, l--)
		{
			t = ac->string[i];
			ac->string[i] = ac->string[l];
			ac->string[l] = t;
		}
	}

	/* Create the affix lists */
	ac = AFCLASS(afdict, AFDICT_INFIXMARK);
	if ((1 < ac->length) || ((1 == ac->length) && (1 != strlen(ac->string[0]))))
	{
		prt_error("Error: afdict_init: Invalid value for class %s in file %s"
		          " (should have been one ASCII punctuation - ignored)\n",
		          afdict_classname[AFDICT_INFIXMARK], afdict->name);
		free((void *)ac->string);
		ac->length = 0;
		ac->mem_elems = 0;
		ac->string = NULL;
	}
	/* XXX For now there is a possibility to use predefined SUF and PRE lists.
	 * So if SUF or PRE are defined, don't extract any of them from the dict. */
	if (1 == ac->length)
	{
		if ((0 == AFCLASS(afdict, AFDICT_PRE)->length) &&
		    (0 == AFCLASS(afdict, AFDICT_SUF)->length))
		{
			char last_entry[MAX_WORD+1] = "";
			get_dict_affixes(dict, dict->root, ac->string[0][0], last_entry);
		}
	}
	else
	{
		/* No INFIX_MARK - create a dummy one that always mismatches */
		affix_list_add(afdict, &afdict->afdict_class[AFDICT_INFIXMARK], "");
	}

	if (verbosity_level(+D_AI))
	{
		size_t l;

		for (ac = afdict->afdict_class;
		     ac < &afdict->afdict_class[ARRAY_SIZE(afdict_classname)]; ac++)
		{
				if (0 == ac->length) continue;
				lgdebug(+0, "Class %s, %zd items:",
				        afdict_classname[ac-afdict->afdict_class], ac->length);
				for (l = 0; l < ac->length; l++)
					lgdebug(0, " '%s'", ac->string[l]);
				lgdebug(0, "\n");
		}
	}
#undef D_AI

	/* Store the SANEMORPHISM regex in the unused (up to now)
	 * regex_root element of the affix dictionary, and precompile it */
	assert(NULL == afdict->regex_root, "SM regex is already assigned");
	ac = AFCLASS(afdict, AFDICT_SANEMORPHISM);
	if (0 != ac->length)
	{
		int rc;

		Regex_node *sm_re = malloc(sizeof(*sm_re));
		dyn_str *rebuf = dyn_str_new();

		/* The regex used to be converted to: ^((original-regex)b)+$
		 * In the initial wordgraph version word boundaries are not supported,
		 * so instead it is converted to: ^(original-regex)+$ */
#ifdef WORD_BOUNDARIES
		dyn_strcat(rebuf, "^((");
#else
		dyn_strcat(rebuf, "^(");
#endif
		dyn_strcat(rebuf, ac->string[0]);
#ifdef WORD_BOUNDARIES
		dyn_strcat(rebuf, ")b)+$");
#else
		dyn_strcat(rebuf, ")+$");
#endif
		sm_re->pattern = strdup(rebuf->str);
		dyn_str_delete(rebuf);

		afdict->regex_root = sm_re;
		sm_re->name = strdup(afdict_classname[AFDICT_SANEMORPHISM]);
		sm_re->re = NULL;
		sm_re->next = NULL;
		sm_re->neg = false;
		rc = compile_regexs(afdict->regex_root, afdict);
		if (rc) {
			prt_error("Error: afdict_init: Failed to compile "
			          "regex '%s' in file %s, return code %d\n",
			          afdict_classname[AFDICT_SANEMORPHISM], afdict->name, rc);
			return false;
		}
		lgdebug(+5, "%s regex %s\n",
		        afdict_classname[AFDICT_SANEMORPHISM], sm_re->pattern);
	}

	/* sort the UNITS list */
	/* Longer unit names must get split off before shorter ones.
	 * This prevents single-letter splits from screwing things
	 * up. e.g. split 7gram before 7am before 7m
	 */
	ac = AFCLASS(afdict, AFDICT_UNITS);
	if (0 < ac->length)
	{
		qsort(ac->string, ac->length, sizeof(char *), cmplen);
	}

#ifdef AFDICT_ORDER_NOT_PRESERVED
	/* pre-sort the MPRE list */
	ac = AFCLASS(afdict, AFDICT_MPRE);
	if (0 < ac->length)
	{
		/* Longer subwords have priority over shorter ones,
		 * reverse-sort by length.
		 * XXX mprefix_split() for Hebrew depends on that. */
		qsort(ac->string, ac->length, sizeof(char *), revcmplen);
	}
#endif /* AFDICT_ORDER_NOT_PRESERVED */

	concat_class(afdict, AFDICT_QUOTES);
	concat_class(afdict, AFDICT_BULLETS);

	if (! anysplit_init(afdict)) return false;

	return true;
}

static void free_llist(Dictionary dict, Dict_node *llist)
{
	free_lookup(llist);
}

/**
 * Dummy lookup function for the affix dictionary -
 * compile_regexs() needs it.
 */
static bool return_true(Dictionary dict, const char *name)
{
	return true;
}

static Dictionary
dictionary_six(const char * lang, const char * dict_name,
               const char * pp_name, const char * cons_name,
               const char * affix_name, const char * regex_name);
/**
 * Read dictionary entries from a wide-character string "input".
 * All other parts are read from files.
 */
#define D_DICT 10
static Dictionary
dictionary_six_str(const char * lang,
                   const char * input,
                   const char * dict_name,
                   const char * pp_name, const char * cons_name,
                   const char * affix_name, const char * regex_name)
{
	const char * t;
	Dictionary dict;
	Dict_node *dict_node;

	dict = (Dictionary) xalloc(sizeof(struct Dictionary_s));
	memset(dict, 0, sizeof(struct Dictionary_s));

	/* Language and file-name stuff */
	dict->string_set = string_set_create();
	t = strrchr (lang, '/');
	t = (NULL == t) ? lang : t+1;
	dict->lang = string_set_add(t, dict->string_set);
	lgdebug(D_USER_FILES, "Debug: Language: %s\n", dict->lang);
	dict->name = string_set_add(dict_name, dict->string_set);

	/*
	 * A special setup per dictionary type. The check here assumes the affix
	 * dictionary name contains "affix". FIXME: For not using this
	 * assumption, the dictionary creating stuff needs a rearrangement.
	 */
	if (0 == strstr(dict->name, "affix"))
	{
		/* To disable spell-checking, just set the checker to NULL */
		dict->spell_checker = spellcheck_create(dict->lang);
#if defined HAVE_HUNSPELL || defined HAVE_ASPELL
		/* FIXME: Move to spellcheck-*.c */
		if (verbosity_level(D_USER_BASIC) && (NULL == dict->spell_checker))
			prt_error("Info: %s: Spell checker disabled.\n", dict->lang);
#endif
		dict->insert_entry = insert_list;

		dict->lookup_list = lookup_list;
		dict->free_lookup = free_llist;
		dict->lookup = boolean_lookup;
	}
	else
	{
		/*
		 * Affix dictionary.
		 */
		size_t i;

		dict->insert_entry = load_affix;
		dict->lookup = return_true;

		/* initialize the class table */
		dict->afdict_class =
		   malloc(sizeof(*dict->afdict_class) * ARRAY_SIZE(afdict_classname));
		for (i = 0; i < ARRAY_SIZE(afdict_classname); i++)
		{
			dict->afdict_class[i].mem_elems = 0;
			dict->afdict_class[i].length = 0;
			dict->afdict_class[i].string = NULL;
		}
	}
	dict->affix_table = NULL;

	/* Read dictionary from the input string. */
	dict->input = input;
	dict->pin = dict->input;
	if (!read_dictionary(dict))
	{
		dict->pin = NULL;
		dict->input = NULL;
		goto failure;
	}
	dict->pin = NULL;
	dict->input = NULL;

	if (NULL == affix_name)
	{
		/*
		 * The affix table is handled alone in this invocation.
		 * Skip the rest of processing!
		 * FIXME: The dictionary creating stuff needs a rearrangement.
		 */
		return dict;
	}

	/* Get the locale for the dictionary. The first one of the
	 * following which exists, is used:
	 * 1. The locale which is defined in the dictionary.
	 * 2. The locale from the environment.
	 * 3. On Windows - the user's default locale.
	 * NULL is returned if the locale is not valid.
	 * Note:
	 * If we don't have locale_t, as a side effect of checking the locale
	 * it is set as the program's locale (as desired).  However, in that
	 * case if it is not valid and this is the first dictionary which is
	 * opened, the program's locale may remain the initial one, i.e. "C"
	 * (unless the API user changed it). */
	dict->locale = linkgrammar_get_dict_locale(dict);

	/* If the program's locale doesn't have a UTF-8 codeset (e.g. it is
	 * "C", or because the API user has set it incorrectly) set it to one
	 * that has it. */
	set_utf8_program_locale();

	/* If the dictionary locale couldn't be established - then set
	 * dict->locale so that it is consistent with the current program's
	 * locale.  It will be used as the intended locale of this
	 * dictionary, and the locale of the compiled regexs. */
	if (NULL == dict->locale)
	{
		dict->locale = setlocale(LC_CTYPE, NULL);
		prt_error("Warning: Couldn't set dictionary locale! "
		          "Using current program locale \"%s\"\n", dict->locale);
	}

	/* setlocale() returns a string owned by the system. Copy it. */
	dict->locale = string_set_add(dict->locale, dict->string_set);


#ifdef HAVE_LOCALE_T
	/* Since linkgrammar_get_dict_locale() (which is called above)
	 * validates the locale, the following call is guaranteed to succeed. */
	dict->lctype = newlocale_LC_CTYPE(dict->locale);

	/* If dict->locale is still not set, there is a bug.
	 * Without this assert(), the program may SEGFAULT when it
	 * uses the isw*() functions. */
	assert((locale_t) 0 != dict->lctype, "Dictionary locale is not set.");
#else
	dict->lctype = 0;
#endif /* HAVE_LOCALE_T */

	/* setlocale() returns a string owned by the system. Copy it. */
	dict->locale = string_set_add(dict->locale, dict->string_set);

	dict->affix_table = dictionary_six(lang, affix_name, NULL, NULL, NULL, NULL);
	if (dict->affix_table == NULL)
	{
		prt_error("Error: Could not open affix file %s\n", affix_name);
		goto failure;
	}
	if (! afdict_init(dict))
		goto failure;

	/*
	 * Process the regex file.
	 * We have to compile regexs using the dictionary locale,
	 * so make a temporary locale swap.
	 */
	if (read_regex_file(dict, regex_name)) goto failure;

	const char *locale = setlocale(LC_CTYPE, NULL); /* Save current locale. */
	locale = strdupa(locale); /* setlocale() uses its own memory. */
	setlocale(LC_CTYPE, dict->locale);
	lgdebug(+D_DICT, "Regexs locale \"%s\"\n", setlocale(LC_CTYPE, NULL));

	if (compile_regexs(dict->regex_root, dict))
	{
		locale = setlocale(LC_CTYPE, locale);         /* Restore the locale. */
		goto failure;
	}
	locale = setlocale(LC_CTYPE, locale);            /* Restore the locale. */
	assert(NULL != locale, "Cannot restore program locale");

#ifdef USE_CORPUS
	dict->corpus = lg_corpus_new();
#endif

	dict->left_wall_defined  = boolean_dictionary_lookup(dict, LEFT_WALL_WORD);
	dict->right_wall_defined = boolean_dictionary_lookup(dict, RIGHT_WALL_WORD);

	dict->base_knowledge  = pp_knowledge_open(pp_name);
	if (NULL == dict->base_knowledge) goto failure;
	dict->hpsg_knowledge  = pp_knowledge_open(cons_name);
	if (NULL == dict->hpsg_knowledge) goto failure;

	dict->unknown_word_defined = boolean_dictionary_lookup(dict, UNKNOWN_WORD);
	dict->use_unknown_word = true;

	dict->shuffle_linkages = false;
	if (0 == strcmp(dict->lang, "any") || NULL != dict->anysplit)
		dict->shuffle_linkages = true;

	dict_node = dictionary_lookup_list(dict, UNLIMITED_CONNECTORS_WORD);
	if (dict_node != NULL)
		dict->unlimited_connector_set = connector_set_create(dict_node->exp);

	free_lookup(dict_node);

	return dict;

failure:
	dictionary_delete(dict);
	return NULL;
}

/**
 * Use filenames of six different files to put together the dictionary.
 */
static Dictionary
dictionary_six(const char * lang, const char * dict_name,
               const char * pp_name, const char * cons_name,
               const char * affix_name, const char * regex_name)
{
	Dictionary dict;

	char* input = get_file_contents(dict_name);
	if (NULL == input)
	{
		prt_error("Error: Could not open dictionary \"%s\"\n", dict_name);
		return NULL;
	}

	dict = dictionary_six_str(lang, input, dict_name, pp_name,
	                          cons_name, affix_name, regex_name);

	free(input);
	return dict;
}

Dictionary dictionary_create_from_file(const char * lang)
{
	Dictionary dictionary;

	init_memusage();
	if (lang && *lang)
	{
		char * dict_name;
		char * pp_name;
		char * cons_name;
		char * affix_name;
		char * regex_name;

		dict_name = join_path(lang, "4.0.dict");
		pp_name = join_path(lang, "4.0.knowledge");
		cons_name = join_path(lang, "4.0.constituent-knowledge");
		affix_name = join_path(lang, "4.0.affix");
		regex_name = join_path(lang, "4.0.regex");

		dictionary = dictionary_six(lang, dict_name, pp_name, cons_name,
		                            affix_name, regex_name);

		free(regex_name);
		free(affix_name);
		free(cons_name);
		free(pp_name);
		free(dict_name);
	}
	else
	{
		prt_error("Error: No language specified!\n");
		dictionary = NULL;
	}

	return dictionary;
}


/**
 * Use "string" as the input dictionary. All of the other parts,
 * including post-processing, affix table, etc, are NULL.
 * This routine is intended for unit-testing ONLY.
 */
Dictionary dictionary_create_from_utf8(const char * input)
{
	Dictionary dictionary = NULL;
	char * lang;

	init_memusage();

	lang = get_default_locale();
	if (lang && *lang) {
		dictionary = dictionary_six_str(lang, input, "string",
		                                NULL, NULL, NULL, NULL);
		free(lang);
	} else {
		/* Default to en when locales are broken (e.g. WIN32) */
		dictionary = dictionary_six_str("en", input, "string",
		                                NULL, NULL, NULL, NULL);
	}

	return dictionary;
}

