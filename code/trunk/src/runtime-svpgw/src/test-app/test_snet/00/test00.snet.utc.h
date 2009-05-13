/**
 * @file test00.h
 *
 * Header definitions of compiled snet-file for runtime.
 *
 * THIS FILE HAS BEEN GENERATED.
 * DO NOT EDIT THIS FILE.
 * EDIT THE ORIGINAL SNET-SOURCE FILE test00.snet INSTEAD!
 *
 * ALL CHANGES MADE TO THIS FILE WILL BE OVERWRITTEN!
 *
*/

#ifndef _TEST00_H_
#define _TEST00_H_

#include "snetentities.h"

#define E__test00__NONE 0
#define F__test00__a 1
#define F__test00__b 2
#define F__test00__c 3
#define F__test00__d 4
#define F__test00__e 5
#define F__test00__f 6
#define T__test00__T 7
#define T__test00__I 8

#define SNET__test00__NUMBER_OF_LABELS 7



#define SNET__test00__NUMBER_OF_INTERFACES 0


extern const char *snet_test00_labels[];

extern const char *snet_test00_interfaces[];


extern snet_tl_stream_t *SNet__test00___test00(snet_tl_stream_t *in_buf);

#endif
