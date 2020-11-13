/*
 *  Copyright (c) 2020 International Business Machines
 *  All rights reserved.
 *
 *  SPDX-License-Identifier: BSD-3-Clause
 *
 *  Authors: Nikolas Ioannou (nio@zurich.ibm.com),
 *           Kornilios Kourtis (kou@zurich.ibm.com, kornilios@gmail.com)
 *
 */
#ifndef _SALSA_DEBUG_H_
#define _SALSA_DEBUG_H_

#include <assert.h>
#include <stdio.h>
#define ERR(format, arg...)				\
	do {						\
		fprintf(stderr, "%s:%d:  " format "\n",	\
			__FUNCTION__, __LINE__, ##arg); \
	} while (0)
#ifdef	DEBUG
#define DBG(format, arg...)				\
	do {						\
		fprintf(stderr, "%s:%d:  " format "\n",	\
			__FUNCTION__, __LINE__, ##arg); \
	} while (0)
#define	DBG_ASSERT(COND)	assert(COND)
#else  /* DEBUG */
#define DBG(format, ...)
#define	DBG_ASSERT(COND)
#endif	/* DEBUG */

#ifndef NDEBUG
#define MSG(format, arg...)				\
	do {						\
		fprintf(stdout, format "\n", ##arg);	\
		fflush(stdout);				\
	} while (0)
#else
#define MSG(format, arg...) do { } while(0)
#endif

// no newline
#define MSG_(format, arg...)				\
	do {						\
		fprintf(stdout, format, ##arg);	\
		fflush(stdout);				\
	} while (0)

#ifndef	BUG
#define BUG()	do {				\
	assert(0);				\
	} while (0)
#endif	/* BUG */

#ifndef	BUG_ON
#define BUG_ON(x)	assert(!(x))
#endif	/* BUG */

#define	ERR_CHK_SET_PRNT_GOTO(cond, var, err, label, msg, ...)	\
	do {							\
		if (unlikely((cond))) {				\
			ERR(msg, ##__VA_ARGS__);		\
			(var) = (err);				\
			goto label;				\
		}						\
	} while(0)

#define	ERR_IF(_cond, _do, _msg, ...)	\
	do {							\
		if (unlikely((_cond))) {			\
			ERR(_msg, ##__VA_ARGS__);		\
			_do;					\
		}						\
	} while(0)

#define	ERR_CHK_SET2_PRNT_GOTO(cond, var, err, var2, err2, label, msg, ...) \
	do {								\
		if (unlikely((cond))) {					\
			ERR(msg, ##__VA_ARGS__);			\
			(var) = (err);					\
			(var2) = (err2);				\
			goto label;					\
		}							\
	} while(0)

#define	ERR_CHK_SET2_GOTO(cond, var, err, var2, err2, label)	\
	do {							\
		if (unlikely((cond))) {				\
			(var) = (err);				\
			(var2) = (err2);			\
			ERR("here");					\
			goto label;				\
		}						\
	} while(0)

#define	ERR_CHK_PRNT_GOTO(cond, label, msg, ...)		\
	do {						\
		if (unlikely((cond))) {			\
			ERR(msg, ##__VA_ARGS__);	\
			goto label;			\
		}					\
	} while(0)

#define	ERR_CHK_PRNT(cond, msg, ...)		\
	do {						\
		if (unlikely((cond))) {			\
			ERR(msg, ##__VA_ARGS__);	\
		}					\
	} while(0)

#define	ERR_CHK_GOTO(cond, label)		\
		do {				\
			if (unlikely((cond)))	\
				goto label;	\
		} while(0)

#define	ERR_CHK_SET_GOTO(cond, var, err, label)	\
		do {				\
			if (unlikely((cond))) {	\
				(var) = (err);	\
				goto label;	\
			}			\
		} while(0)

#define	ERR_SET_GOTO(var, err, label)		\
		do {				\
			(var) = (err);		\
			goto label;		\
		} while(0)

#define	ERR_SET_PRNT_GOTO(var, err, label, msg, ...)	\
		do {					\
			ERR(msg, ##__VA_ARGS__);	\
			(var) = (err);			\
			goto label;			\
		} while(0)

#define	ERR_CHK_SET_PRNTDBG_GOTO(cond, var, err, label, msg, ...)	\
	do {							\
		if (unlikely((cond))) {				\
			DBG(msg, ##__VA_ARGS__);		\
			(var) = (err);				\
			goto label;				\
		}						\
	} while(0)

#define	ERR_CHK_PRNTDBG_GOTO(cond, label, msg, ...)	\
	do {						\
		if (unlikely((cond))) {			\
			DBG(msg, ##__VA_ARGS__);	\
			goto label;			\
		}					\
	} while(0)

#define	CHK_GOTO(cond, label)		\
		do {				\
			if ((cond))		\
				goto label;	\
		} while(0)

#define	CHK_SET_GOTO(cond, var, err, label)	\
		do {				\
			if ((cond)) {	\
				(var) = (err);	\
				goto label;	\
			}			\
		} while(0)
#endif	/* _SALSA_DEBUG_H_ */
