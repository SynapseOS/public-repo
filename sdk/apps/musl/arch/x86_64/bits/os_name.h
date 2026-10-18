#ifndef _BITS_OS_NAME_H
#define _BITS_OS_NAME_H

// -D__SYNAPSE__ из Makefile
#ifdef __SYNAPSE__
#define SYNAPSE_OS 1
#else
#define SYNAPSE_OS 0
#endif

#endif