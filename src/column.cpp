//File: $Id$
// Author: John Wu <John.Wu at ACM.org>
// Copyright 2000-2009 the Regents of the University of California
//
// This file contains implementatioin of the functions defined in column.h
//
#include "resource.h"	// ibis::resource, ibis::gParameters()
#include "category.h"	// ibis::text, ibis::category
#include "column.h"	// ibis::column
#include "part.h"	// ibis::part
#include "iroster.h"	// ibis::roster

#include <stdarg.h>	// vsprintf
#include <ctype.h>	// tolower
#include <limits>	// std::numeric_limits
#include <typeinfo>	// typeid
#include <cmath>	// std::log

#if defined(_WIN32) && defined(_MSC_VER)
#pragma warning(disable:4786)	// some identifier longer than 256 characters
// needed for numeric_limits<>::max, min function calls
#ifdef max
#undef max
#endif
#ifdef min
#undef min
#endif
#endif

// constants defined for type name and type code used in the metadata file
const char* ibis::TYPECODE   = "?OBAHGIULVRDCS";
static const char* _ibis_TYPESTRING_local[] = {
    "UNKNOWN", "OID", "BYTE", "UBYTE", "SHORT", "USHORT", "INT", "UINT",
    "LONG", "ULONG", "FLOAT", "DOUBLE", "CATEGORY", "TEXT"
};
const char** ibis::TYPESTRING = _ibis_TYPESTRING_local;

/// Construct a new column object based on type and name.
ibis::column::column(const ibis::part* tbl, ibis::TYPE_T t,
		     const char* name, const char* desc,
		     double low, double high) :
    thePart(tbl), m_type(t), m_name(name), m_desc(desc), m_bins(""),
    m_sorted(false), lower(low), upper(high), idx(0), idxcnt() {
    if (0 != pthread_rwlock_init(&rwlock, 0)) {
	throw "ibis::column::ctor unable to initialize the rwlock";
    }
    if (0 != pthread_mutex_init
	(&mutex, static_cast<const pthread_mutexattr_t*>(0))) {
	throw "ibis::column::ctor unable to initialize the mutex";
    }
    if (m_desc.empty()) m_desc = name;
    LOGGER(ibis::gVerbose > 5 && !m_name.empty() && thePart != 0)
	<< "initialized column " << name << " for partition " << tbl->name();
} // ibis::column::column

/// Read the basic information about a column from file.
///@note
/// Assume the calling program has read "Begin Property/Column" already.
///@note
/// A well-formed column must have a valid name, i.e., ! m_name.empty().
ibis::column::column(const part* tbl, FILE* file)
    : thePart(tbl), m_type(UINT), m_sorted(false), lower(DBL_MAX),
      upper(-DBL_MAX), idx(0), idxcnt() {
    char buf[MAX_LINE];
    char *s1;
    char *s2;

    if (0 != pthread_rwlock_init(&rwlock, 0)) {
	throw "ibis::column::ctor unable to initialize the rwlock";
    }
    if (0 != pthread_mutex_init
	(&mutex, static_cast<const pthread_mutexattr_t *>(0))) {
	throw "ibis::column::ctor unable to initialize the mutex";
    }

    bool badType = false;
    // read the column entry of the metadata file
    // assume the calling program has read "Begin Property/Column" already
    do {
	s1 = fgets(buf, MAX_LINE, file);
	if (s1 == 0) {
	    ibis::util::logMessage("Warning", "ibis::column::ctor reached "
				   "end-of-file while reading a column");
	    return;
	}
	if (strlen(buf) + 1 >= MAX_LINE) {
	    ibis::util::logMessage("Warning", "ibis::column::ctor may "
				   "have encountered a line that has more "
				   "than %d characters", MAX_LINE);
	}

	s1 = strchr(buf, '=');
	if (s1!=0 && s1[1]!=static_cast<char>(0)) ++s1;
	else s1 = 0;

	if (buf[0] == '#') {
	    // skip the comment line
	}
	else if (strnicmp(buf, "name", 4) == 0 ||
		 strnicmp(buf, "Property_name", 13) == 0) {
	    s2 = ibis::util::getString(s1);
	    m_name = s2;
	    delete [] s2;
	}
	else if (strnicmp(buf, "description", 11) == 0 ||
		 strnicmp(buf, "Property_description", 20) == 0) {
	    s2 = ibis::util::getString(s1);
	    m_desc = s2;
	    delete [] s2;
	}
	else if (strnicmp(buf, "minimum", 7) == 0) {
	    s1 += strspn(s1, " \t=\'\"");
	    lower = atof(s1);
	}
	else if (strnicmp(buf, "maximum", 7) == 0) {
	    s1 += strspn(s1, " \t=\'\"");
	    upper = atof(s1);
	}
	else if (strnicmp(buf, "Bins:", 5) == 0) {
	    s1 = buf + 5;
	    s1 += strspn(s1, " \t");
	    s2 = s1 + strlen(s1) - 1;
	    while (s2>=s1 && isspace(*s2)) {
		*s2 = static_cast<char>(0);
		--s2;
	    }
#if defined(INDEX_SPEC_TO_LOWER)
	    s2 = s1 + strlen(s1) - 1;
	    while (s2 >= s1) {
		*s2 = tolower(*s2);
		-- s2;
	    }
#endif
	    m_bins = s1;
	}
	else if (strnicmp(buf, "Index", 5) == 0) {
	    s1 = ibis::util::getString(s1);
#if defined(INDEX_SPEC_TO_LOWER)
	    s2 = s1 + strlen(s1) - 1;
	    while (s2 >= s1) {
		*s2 = tolower(*s2);
		-- s2;
	    }
#endif
	    m_bins = s1;
	    delete [] s1;
	}
	else if (strnicmp(buf, "sorted", 6) == 0 && s1 != 0 && *s1 != 0) {
	    while (s1 != 0 && *s1 != 0 && isspace(*s1))
		++ s1;
	    if (s1 != 0 && *s1 != 0)
		m_sorted = ibis::resource::isStringTrue(s1);
	}
	else if (strnicmp(buf, "Property_data_type", 18) == 0 ||
		 strnicmp(buf, "data_type", 9) == 0 ||
		 strnicmp(buf, "type", 4) == 0) {
	    s1 += strspn(s1, " \t=\'\"");

	    switch (*s1) {
	    case 'i':
	    case 'I': { // can only be INT
		m_type = ibis::INT;
		break;}
	    case 'u':
	    case 'U': { // defaults to UINT, but may be other unsigned
		m_type = ibis::UINT;
		if (s1[1] == 's' || s1[1] == 'S') { // USHORT
		    m_type = ibis::USHORT;
		}
		else if (s1[1] == 'b' || s1[1] == 'B' ||
			 s1[1] == 'c' || s1[1] == 'C') { // UBYTE
		    m_type = ibis::UBYTE;
		}
		else if (s1[1] == 'l' || s1[1] == 'L') { // ULONG
		    m_type = ibis::ULONG;
		}
		else if (s1[1] == 'n' || s1[1] == 'N') { // unsigned xxx
		    s1 += 8; // skip "unsigned"
		    s1 += strspn(s1, " \t=\'\""); // skip space
		    if (*s1 == 's' || *s1 == 'S') { // USHORT
			m_type = ibis::USHORT;
		    }
		    else if (*s1 == 'b' || *s1 == 'B' ||
			     *s1 == 'c' || *s1 == 'C') { // UBYTE
			m_type = ibis::UBYTE;
		    }
		    else if (*s1 == 'l' || *s1 == 'L') { // ULONG
			m_type = ibis::ULONG;
		    }
		}
		break;}
	    case 'r':
	    case 'R': { // FLOAT
		m_type = ibis::FLOAT;
		break;}
	    case 'f':
	    case 'F': {// FLOAT
		m_type = ibis::FLOAT;
		break;}
	    case 'd':
	    case 'D': { // DOUBLE
		m_type = ibis::DOUBLE;
		break;}
	    case 'c':
	    case 'C':
	    case 'k':
	    case 'K': { // KEY
		m_type = ibis::CATEGORY;
		break;}
	    case 's':
	    case 'S': { // default to string, but could be short
		m_type = ibis::TEXT;
		if (s1[1] == 'h' || s1[1] == 'H')
		    m_type = ibis::SHORT;
		break;}
	    case 't':
	    case 'T': {
		m_type = ibis::TEXT;
		break;}
	    case 'a':
	    case 'A': { // UBYTE
		m_type = ibis::UBYTE;
		break;}
	    case 'b':
	    case 'B': { // BYTE
		m_type = ibis::BYTE;
		break;}
	    case 'g':
	    case 'G': { // USHORT
		m_type = ibis::USHORT;
		break;}
	    case 'H':
	    case 'h': { // short, half word
		m_type = ibis::SHORT;
		break;}
	    case 'l':
	    case 'L': { // LONG (int64_t)
		m_type = ibis::LONG;
		break;}
	    case 'v':
	    case 'V': { // unsigned long (uint64_t)
		m_type = ibis::ULONG;
		break;}
	    default: {
		ibis::util::logMessage("Warning",
				       "ibis::column::ctor encountered "
				       "unknown data type \'%s\'", s1);
		badType = true;
		break;}
	    }
	}
	else if (strnicmp(buf, "End", 3) && ibis::gVerbose > 4){
	    ibis::util::logMessage("ibis::column::column",
				   "skipping line:\n%s", buf);
	}
    } while (strnicmp(buf, "End", 3));

    if (m_name.empty() || badType) {
	ibis::util::logMessage("Warning",
			       "ibis::column specification does not have a "
			       "valid name or type");
	m_name.erase(); // make sure the name is empty
    }
    LOGGER(ibis::gVerbose > 5 && thePart != 0)
	<< "read info about column "
	<< (m_name.empty() ? "???" : m_name.c_str()) << " for partition "
	<< thePart->name();
} // ibis::column::column

/// The copy constructor.
/// @note The rwlock can not be copied.
/// @note The index is not copied either because reference counting
/// difficulties.
ibis::column::column(const ibis::column& rhs) :
    thePart(rhs.thePart), m_type(rhs.m_type), m_name(rhs.m_name),
    m_desc(rhs.m_desc), m_bins(rhs.m_bins), m_sorted(false), lower(rhs.lower),
    upper(rhs.upper), idx(0), idxcnt() {
    if (pthread_rwlock_init(&rwlock, 0)) {
	throw "ibis::column::ctor unable to initialize the rwlock";
    }
    if (pthread_mutex_init(&mutex, 0)) {
	throw "ibis::column::ctor unable to initialize the mutex";
    }
    LOGGER(ibis::gVerbose > 5 && !m_name.empty() && thePart != 0)
	<< "made a new copy of column "
	<< (m_name.empty() ? "???" : m_name.c_str());
} // copy constructor

ibis::column::~column() {
    { // must not be used for anything else
	mutexLock mk(this, "~column");
	writeLock wk(this, "~column");
	delete idx;
	LOGGER(ibis::gVerbose > 5 && !m_name.empty() && thePart != 0)
	    << "clearing column " << thePart->name() << '.' << m_name;
    }

    pthread_mutex_destroy(&mutex);
    pthread_rwlock_destroy(&rwlock);
} // destructor

/// Write the current content to the metadata file -part.txt of the data
/// partition.
void ibis::column::write(FILE* file) const {
    fputs("\nBegin Column\n", file);
    fprintf(file, "name = \"%s\"\n", (const char*)m_name.c_str());
    if (! m_desc.empty())
	fprintf(file, "description =\"%s\"\n",
		(const char*)m_desc.c_str());
    fprintf(file, "data_type = \"%s\"\n", ibis::TYPESTRING[(int)m_type]);
    if (upper >= lower) {
	switch (m_type) {
	case BYTE:
	case SHORT:
	case INT:
	    fprintf(file, "minimum = %ld\n", static_cast<long>(lower));
	    fprintf(file, "maximum = %ld\n", static_cast<long>(upper));
	    break;
	case FLOAT:
	    fprintf(file, "minimum = %.8g\n", lower);
	    fprintf(file, "maximum = %.8g\n", upper);
	    break;
	case DOUBLE:
	    fprintf(file, "minimum = %.15g\n", lower);
	    fprintf(file, "maximum = %.15g\n", upper);
	    break;
	default: // Uxxx, CATEGORY, TEXT, ...
	    fprintf(file, "minimum = %lu\n",
		    static_cast<long unsigned>(lower));
	    fprintf(file, "maximum = %lu\n",
		    static_cast<long unsigned>(upper));
	    break;
	}
    }
    if (! m_bins.empty())
	fprintf(file, "index = %s\n", m_bins.c_str());
    if (m_sorted)
	fprintf(file, "sorted = true\n");
    fputs("End Column\n", file);
} // ibis::column::write

const char* ibis::column::indexSpec() const {
    return (m_bins.empty() ? (thePart ? thePart->indexSpec() : 0)
	    : m_bins.c_str());
}

uint32_t ibis::column::numBins() const {
    uint32_t nBins = 0;
    //      if (idx)
    //  	nBins = idx->numBins();
    if (nBins == 0) { //  read the no= field in m_bins
	const char* str = strstr(m_bins.c_str(), "no=");
	if (str == 0) {
	    str = strstr(m_bins.c_str(), "NO=");
	    if (str == 0) {
		str = strstr(m_bins.c_str(), "No=");
	    }
	    if (str == 0 && thePart != 0 && thePart->indexSpec() != 0) {
		str = strstr(thePart->indexSpec(), "no=");
		if (str == 0) {
		    str = strstr(thePart->indexSpec(), "NO=");
		    if (str == 0)
			str = strstr(thePart->indexSpec(), "No=");
		}
	    }
	}
	if (str) {
	    str += 3;
	    nBins = atoi(str);
	}
    }
    if (nBins == 0)
	nBins = 10;
    return nBins;
} // ibis::column::numBins

// actually go through values and determine the min/max values, the min is
// recordeed as lowerBound and the max is recorded as the upperBound
void ibis::column::computeMinMax() {
    if (thePart == 0 || thePart->currentDataDir() == 0) return;
    std::string sname;
    const char* name = dataFileName(sname);
    if (name != 0) {
	ibis::bitvector msk;
	getNullMask(msk);
	actualMinMax(name, msk, lower, upper);
    }
} // ibis::column::computeMinMax

void ibis::column::computeMinMax(const char *dir) {
    if (dir == 0 && (thePart == 0 || thePart->currentDataDir() == 0)) return;
    std::string sname;
    const char* name = dataFileName(sname, dir);
    if (name != 0) {
	ibis::bitvector msk;
	getNullMask(msk);
	actualMinMax(name, msk, lower, upper);
    }
} // ibis::column::computeMinMax

/// Go through the values in data directory @c dir and compute the actual
/// min and max.
void ibis::column::computeMinMax(const char *dir, double &min,
				 double &max) const {
    if (dir == 0 && (thePart == 0 || thePart->currentDataDir() == 0)) return;
    std::string sname;
    const char* name = dataFileName(sname, dir);
    if (name != 0) {
	ibis::bitvector msk;
	getNullMask(msk);
	actualMinMax(name, msk, min, max);
    }
    else {
	min = DBL_MAX;
	max = -DBL_MAX;
    }
} // ibis::column::computeMinMax

/// Compute the actual minimum and maximum values.  Given a data file name,
/// read its content to compute the actual minimum and the maximum of the
/// data values.  Only deal with four types of values, unsigned int, signed
/// int, float and double.
void ibis::column::actualMinMax(const char *name, const ibis::bitvector& mask,
				double &min, double &max) const {
    switch (m_type) {
    case ibis::UBYTE: {
	array_t<unsigned char> val;
	int ierr = ibis::fileManager::instance().getFile(name, val);
	if (ierr != 0) {
	    logWarning("actualMinMax", "unable to retrieve file %s", name);
	    return;
	}

	actualMinMax(val, mask, min, max);
	break;}
    case ibis::BYTE: {
	array_t<signed char> val;
	int ierr = ibis::fileManager::instance().getFile(name, val);
	if (ierr != 0) {
	    logWarning("actualMinMax", "unable to retrieve file %s", name);
	    return;
	}
	actualMinMax(val, mask, min, max);
	break;}
    case ibis::USHORT: {
	array_t<uint16_t> val;
	int ierr = ibis::fileManager::instance().getFile(name, val);
	if (ierr != 0) {
	    logWarning("actualMinMax", "unable to retrieve file %s", name);
	    return;
	}

	actualMinMax(val, mask, min, max);
	break;}
    case ibis::SHORT: {
	array_t<int16_t> val;
	int ierr = ibis::fileManager::instance().getFile(name, val);
	if (ierr != 0) {
	    logWarning("actualMinMax", "unable to retrieve file %s", name);
	    return;
	}

	actualMinMax(val, mask, min, max);
	break;}
    case ibis::UINT: {
	array_t<uint32_t> val;
	int ierr = ibis::fileManager::instance().getFile(name, val);
	if (ierr != 0) {
	    logWarning("actualMinMax", "unable to retrieve file %s", name);
	    return;
	}

	actualMinMax(val, mask, min, max);
	break;}
    case ibis::INT: {
	array_t<int32_t> val;
	int ierr = ibis::fileManager::instance().getFile(name, val);
	if (ierr != 0) {
	    logWarning("actualMinMax", "unable to retrieve file %s", name);
	    return;
	}

	actualMinMax(val, mask, min, max);
	break;}
    case ibis::ULONG: {
	array_t<uint64_t> val;
	int ierr = ibis::fileManager::instance().getFile(name, val);
	if (ierr != 0) {
	    logWarning("actualMinMax", "unable to retrieve file %s", name);
	    return;
	}

	actualMinMax(val, mask, min, max);
	break;}
    case ibis::LONG: {
	array_t<int64_t> val;
	int ierr = ibis::fileManager::instance().getFile(name, val);
	if (ierr != 0) {
	    logWarning("actualMinMax", "unable to retrieve file %s", name);
	    return;
	}

	actualMinMax(val, mask, min, max);
	break;}
    case ibis::FLOAT: {
	array_t<float> val;
	int ierr = ibis::fileManager::instance().getFile(name, val);
	if (ierr != 0) {
	    logWarning("actualMinMax", "unable to retrieve file %s", name);
	    return;
	}

	actualMinMax(val, mask, min, max);
	break;}
    case ibis::DOUBLE: {
	array_t<double> val;
	int ierr = ibis::fileManager::instance().getFile(name, val);
	if (ierr != 0) {
	    logWarning("actualMinMax", "unable to retrieve file %s", name);
	    return;
	}

	actualMinMax(val, mask, min, max);
	break;}
    default:
	if (ibis::gVerbose > 2)
	    logMessage("actualMinMax", "column type %s is not one of the "
		       "supported types (int, uint, float, double)",
		       ibis::TYPESTRING[static_cast<int>(m_type)]);
	min = 0;
	max = (thePart != 0) ? thePart->nRows() : -DBL_MAX;
    } // switch(m_type)
} // ibis::column::actualMinMax

/// There is no need for the caller to free this pointer.  In normal case,
/// the pointer returned is fname.c_str(); otherwise, the return value is a
/// nil pointer.
const char*
ibis::column::dataFileName(std::string& fname, const char *dir) const {
    const char *name = 0;
    if (dir == 0 && (thePart == 0 || thePart->currentDataDir() == 0))
	return name;

    const char *adir = ((dir != 0 && *dir != 0) ? dir :
			thePart!=0 ? thePart->currentDataDir() : 0);
    if (adir != 0 && *adir != 0) {
	fname = adir;
	if (fname[fname.size()-1] != FASTBIT_DIRSEP)
	    fname += FASTBIT_DIRSEP;
	fname += m_name;
	name = fname.c_str();
    }
    return name;
} // ibis::column::dataFileName

/// On successful completion of this function, the return value is the
/// result of fname.c_str(); otherwise the return value is a nil pointer to
/// indicate error.
const char* ibis::column::nullMaskName(std::string& fname) const {
    if (thePart == 0 || thePart->currentDataDir() == 0) return 0;

    fname = thePart->currentDataDir();
    fname += FASTBIT_DIRSEP;
    fname += m_name;
    fname += ".msk";
    return fname.c_str();
} // ibis::column::nullMaskName

/// If there is a null mask stored already, return a shallow copy of it in
/// mask.  Otherwise, find out the size of the data file first, if the
/// actual content of the null mask file has less bits, assume the mask is
/// for the leading portion of the data file and the remaining portion of
/// the data file is valid (not null).
void ibis::column::getNullMask(ibis::bitvector& mask) const {
    if (thePart == 0) return;

    mutexLock lock(this, "column::getNullMask");
    if (mask_.size() == thePart->nRows()) {
	ibis::bitvector tmp(mask_);
	mask.swap(tmp);
#if defined(DEBUG)
	logMessage("getNullMask", "copying an existing mask(%lu, %lu)",
		   static_cast<long unsigned>(mask.cnt()),
		   static_cast<long unsigned>(mask.size()));
#endif
    }
    else if (m_type == ibis::OID) {
	const_cast<column*>(this)->mask_.set(1, thePart->nRows());
	mask.set(1, thePart->nRows());
#if defined(DEBUG)
	logMessage("getNullMask", "asking for the mask of OIDs (%lu, %lu)",
		   static_cast<long unsigned>(mask.cnt()),
		   static_cast<long unsigned>(mask.size()));
#endif
    }
    else {
	Stat_T st;
	std::string sname;
	const char* fnm = 0;
	array_t<ibis::bitvector::word_t> arr;
	fnm = dataFileName(sname);
	if (fnm != 0 && UnixStat(fnm, &st) == 0) {
	    const uint32_t elm = elementSize();
	    uint32_t sz = (elm > 0 ? st.st_size / elm :  thePart->nRows());

	    // get the null mask file name and read the file
	    fnm = nullMaskName(sname);
	    int ierr = -1;
	    try {
		ierr = ibis::fileManager::instance().getFile(fnm, arr);
		if (ierr == 0) {
		    // arr is implicitly converted to a bitvector
		    mask.copy(arr);
		}
		else {
		    mask.set(1, sz);
		}
	    }
	    catch (...) {
		mask.set(1, sz);
	    }

	    if (mask.size() != thePart->nRows() &&
		thePart->getStateNoLocking() == ibis::part::STABLE_STATE) {
		mask.adjustSize(sz, thePart->nRows());
		ibis::fileManager::instance().flushFile(fnm);
		mask.write(fnm);
		LOGGER(ibis::gVerbose > 1)
		    << "column[" << thePart->name() << '.' << m_name
		    << "]::getNullMask constructed a new mask with "
		    << mask.cnt() << " out of " << mask.size()
		    << " set bits, wrote to " << fnm;
	    }
	    if (ibis::gVerbose > 3)
		logMessage("getNullMask", "get null mask (%lu, %lu) "
			   "[st.st_size=%lu, sz=%lu, ierr=%d]",
			   static_cast<long unsigned>(mask.cnt()),
			   static_cast<long unsigned>(mask.size()),
			   static_cast<long unsigned>(st.st_size),
			   static_cast<long unsigned>(sz), ierr);
	}
	else { // no data file, assume every value is valid
	    mask.set(1, thePart->nRows());
	}

	ibis::bitvector tmp(mask);
	const_cast<column*>(this)->mask_.swap(tmp);
    }
    if (ibis::gVerbose > 6) {
	logMessage("getNullMask", "mask size = %lu, cnt = %lu",
		   static_cast<long unsigned>(mask.size()),
		   static_cast<long unsigned>(mask.cnt()));
    }
} // ibis::column::getNullMask

ibis::array_t<int32_t>* ibis::column::getIntArray() const {
    ibis::array_t<int32_t>* array = 0;
    if (m_type == INT || m_type == UINT) {
	array = new array_t<int32_t>;
	std::string sname;
	const char* fnm = dataFileName(sname);
	if (fnm == 0) return array;

	//ibis::part::readLock lock(thePart, "column::getIntArray");
	int ierr = ibis::fileManager::instance().getFile(fnm, *array);
	if (ierr != 0) {
	    logWarning("getIntArray",
		       "the file manager faild to retrieve the content of"
		       " the data file \"%s\"", fnm);
	}
    }
    else {
	logWarning("getIntArray", "incompatible data type");
    }
    return array;
} // ibis::column::getIntArray

ibis::array_t<float>* ibis::column::getFloatArray() const {
    ibis::array_t<float>* array = 0;
    if (m_type == FLOAT) {
	array = new array_t<float>;
	std::string sname;
	const char* fnm = dataFileName(sname);
	if (fnm == 0) return array;

	//ibis::part::readLock lock(thePart, "column::getFloatArray");
	int ierr = ibis::fileManager::instance().getFile(fnm, *array);
	if (ierr != 0) {
	    logWarning("getFloatArray",
		       "the file manager faild to retrieve the content of"
		       " the data file \"%s\"", fnm);
	}
    }
    else {
	logWarning("getFloatArray()", " incompatible data type");
    }
    return array;
} // ibis::column::getFloatArray

ibis::array_t<double>* ibis::column::getDoubleArray() const {
    ibis::array_t<double>* array = 0;
    if (m_type == DOUBLE) {
	array = new array_t<double>;
	std::string sname;
	const char* fnm = dataFileName(sname);
	if (fnm == 0) return array;

	//ibis::part::readLock lock(thePart, "column::getDoubleArray");
	int ierr = ibis::fileManager::instance().getFile(fnm, *array);
	if (ierr != 0) {
	    logWarning("getDoubleArray",
		       "the file manager faild to retrieve the content of"
		       " the data file \"%s\"", fnm);
	}
    }
    else {
	logWarning("getDoubleArray", "incompatible data type");
    }
    return array;
} // ibis::column::getDoubleArray

/// The incoming argument must be array_t<Type>*.  This function will
/// explicitly cast vals into one of the ten supported numerical data
/// types.
/// 
/// It return 0 to indicate success, and a negative number to indicate
/// error.
int ibis::column::getValuesArray(void* vals) const {
    if (vals == 0) return -1;
    int ierr = 0;
    ibis::fileManager::storage *tmp = getRawData();
    if (tmp != 0) {
	switch (m_type) {
	case ibis::BYTE: {
	    array_t<char> ta(tmp);
	    static_cast<array_t<char>*>(vals)->swap(ta);
	    break;}
	case ibis::UBYTE: {
	    array_t<unsigned char> ta(tmp);
	    static_cast<array_t<unsigned char>*>(vals)->swap(ta);
	    break;}
	case ibis::SHORT: {
	    array_t<int16_t> ta(tmp);
	    static_cast<array_t<int16_t>*>(vals)->swap(ta);
	    break;}
	case ibis::USHORT: {
	    array_t<uint16_t> ta(tmp);
	    static_cast<array_t<uint16_t>*>(vals)->swap(ta);
	    break;}
	case ibis::INT: {
	    array_t<int32_t> ta(tmp);
	    static_cast<array_t<int32_t>*>(vals)->swap(ta);
	    break;}
	case ibis::UINT: {
	    array_t<uint32_t> ta(tmp);
	    static_cast<array_t<uint32_t>*>(vals)->swap(ta);
	    break;}
	case ibis::LONG: {
	    array_t<int64_t> ta(tmp);
	    static_cast<array_t<int64_t>*>(vals)->swap(ta);
	    break;}
	case ibis::ULONG: {
	    array_t<uint64_t> ta(tmp);
	    static_cast<array_t<uint64_t>*>(vals)->swap(ta);
	    break;}
	case ibis::FLOAT: {
	    array_t<float> ta(tmp);
	    static_cast<array_t<float>*>(vals)->swap(ta);
	    break;}
	case ibis::DOUBLE: {
	    array_t<double> ta(tmp);
	    static_cast<array_t<double>*>(vals)->swap(ta);
	    break;}
	case ibis::OID: {
	    array_t<ibis::rid_t> ta(tmp);
	    static_cast<array_t<ibis::rid_t>*>(vals)->swap(ta);
	    break;}
        default: {
	    LOGGER(ibis::gVerbose >= 0)
		<< "column::getValuesArray(" << vals
		<< ") does not support data type "
		<< ibis::TYPESTRING[static_cast<int>(m_type)];
	    ierr = -2;
	    break;}
        }
    }
    else {
	ierr = -3;
    }
    return ierr;
} // ibis::column::getValuesArray

ibis::fileManager::storage* ibis::column::getRawData() const {
    std::string sname;
    const char *fnm = dataFileName(sname);
    if (fnm == 0) return 0;

    ibis::fileManager::storage *res = 0;
    int ierr = ibis::fileManager::instance().getFile(fnm, &res);
    if (ierr != 0) {
	logWarning("getRawData",
		   "the file manager faild to retrieve the content "
		   "of the file \"%s\"", fnm);
	delete res;
	res = 0;
    }
    return res;
} // ibis::column::getRawData

/// Retrieve selected 1-byte integer values.  Note that unsigned
/// integers are simply treated as signed integers.
///
/// @note The caller is responsible for freeing the returned array from any
/// of the selectTypes functions.
ibis::array_t<char>*
ibis::column::selectBytes(const ibis::bitvector& mask) const {
    ibis::array_t<char>* array = new array_t<char>;
    const uint32_t tot = mask.cnt();
    if (tot == 0)
	return array;

    ibis::horometer timer;
    if (ibis::gVerbose > 4)
	timer.start();
    if (m_type == ibis::BYTE || m_type == ibis::UBYTE) {
#if defined(FASTBIT_PREFER_READ_ALL)
	array_t<char> prop;
	std::string sname;
	const char* fnm = dataFileName(sname);
	ibis::fileManager::ACCESS_PREFERENCE apref =
	    thePart != 0 ? thePart->accessHint(mask, sizeof(char))
	    : ibis::fileManager::MMAP_LARGE_FILES;

	int ierr = ibis::fileManager::instance().getFile(fnm, prop, apref);
	if (ierr != 0) {
	    logWarning("selectBytes",
		       "the file manager faild to retrieve the content of"
		       " the data file \"%s\"", fnm);
	    return array;
	}

	uint32_t i = 0;
	array->resize(tot);
	const uint32_t nprop = prop.size();
	ibis::bitvector::indexSet index = mask.firstIndexSet();
	if (nprop >= mask.size()) { // no need to check loop bounds
	    while (index.nIndices() > 0) {
		const ibis::bitvector::word_t *idx0 = index.indices();
		if (index.isRange()) {
		    for (uint32_t j = *idx0; j < idx0[1]; ++j, ++i) {
			(*array)[i] = (prop[j]);
		    }
		}
		else {
		    for (uint32_t j = 0; j<index.nIndices(); ++j, ++i) {
			(*array)[i] = (prop[idx0[j]]);
		    }
		}
		++ index;
	    }
	}
	else { // need to check loop bounds against nprop
	    while (index.nIndices() > 0) {
		const ibis::bitvector::word_t *idx0 = index.indices();
		if (*idx0 >= nprop) break;
		if (index.isRange()) {
		    for (uint32_t j = *idx0;
			 j < (idx0[1]<=nprop ? idx0[1] : nprop);
			 ++j, ++i) {
			(*array)[i] = (prop[j]);
		    }
		}
		else {
		    for (uint32_t j = 0; j<index.nIndices(); ++j, ++i) {
			if (idx0[j] < nprop)
			    (*array)[i] = (prop[idx0[j]]);
			else
			    break;
		    }
		}
		++ index;
	    }
	}
	if (i != tot) {
	    array->resize(i);
	    logWarning("selectBytes", "expects to retrieve %lu elements "
		       "but only got %lu", static_cast<long unsigned>(tot),
		       static_cast<long unsigned>(i));
	}
#else
	long ierr = selectValuesT(mask, *array);
	if (ierr < 0) {
	    LOGGER(ibis::gVerbose > 0)
		<< "Warning -- ibis::column["
		<< (thePart!=0 ? thePart->name() : "") << "." << m_name
		<< "]::selectValuesT failed with error code " << ierr;
	    delete array;
	    array = 0;
	}
#endif
    }
    else {
	logWarning("selectBytes", "incompatible data type");
    }
    if (ibis::gVerbose > 4) {
	timer.stop();
	long unsigned cnt = mask.cnt();
	logMessage("selectBytes", "retrieving %lu integer%s "
		   "took %g sec(CPU), %g sec(elapsed)",
		   static_cast<long unsigned>(cnt), (cnt > 1 ? "s" : ""),
		   timer.CPUTime(), timer.realTime());
    }
    return array;
} // ibis::column::selectBytes

/// @note The caller is responsible for freeing the returned array from any
/// of the selectTypes functions.
ibis::array_t<unsigned char>*
ibis::column::selectUBytes(const ibis::bitvector& mask) const {
    ibis::array_t<unsigned char>* array = new array_t<unsigned char>;
    const uint32_t tot = mask.cnt();
    if (tot == 0)
	return array;

    ibis::horometer timer;
    if (ibis::gVerbose > 4)
	timer.start();
    if (m_type == ibis::BYTE || m_type == ibis::UBYTE) {
#if defined(FASTBIT_PREFER_READ_ALL)
	array_t<unsigned char> prop;
	std::string sname;
	const char* fnm = dataFileName(sname);
	ibis::fileManager::ACCESS_PREFERENCE apref =
	    thePart != 0 ? thePart->accessHint(mask, sizeof(unsigned char))
	    : ibis::fileManager::MMAP_LARGE_FILES;

	int ierr = ibis::fileManager::instance().getFile(fnm, prop, apref);
	if (ierr != 0) {
	    logWarning("selectUBytes",
		       "the file manager faild to retrieve the content of"
		       " the data file \"%s\"", fnm);
	    return array;
	}

	uint32_t i = 0;
	array->resize(tot);
	const uint32_t nprop = prop.size();
	ibis::bitvector::indexSet index = mask.firstIndexSet();
	if (nprop >= mask.size()) { // no need to check loop bounds
	    while (index.nIndices() > 0) {
		const ibis::bitvector::word_t *idx0 = index.indices();
		if (index.isRange()) {
		    for (uint32_t j = *idx0; j < idx0[1]; ++j, ++i) {
			(*array)[i] = (prop[j]);
		    }
		}
		else {
		    for (uint32_t j = 0; j<index.nIndices(); ++j, ++i) {
			(*array)[i] = (prop[idx0[j]]);
		    }
		}
		++ index;
	    }
	}
	else { // need to check loop bounds against nprop
	    while (index.nIndices() > 0) {
		const ibis::bitvector::word_t *idx0 = index.indices();
		if (*idx0 >= nprop) break;
		if (index.isRange()) {
		    for (uint32_t j = *idx0;
			 j < (idx0[1]<=nprop ? idx0[1] : nprop);
			 ++j, ++i) {
			(*array)[i] = (prop[j]);
		    }
		}
		else {
		    for (uint32_t j = 0; j<index.nIndices(); ++j, ++i) {
			if (idx0[j] < nprop)
			    (*array)[i] = (prop[idx0[j]]);
			else
			    break;
		    }
		}
		++ index;
	    }
	}
	if (i != tot) {
	    array->resize(i);
	    logWarning("selectUBytes", "expects to retrieve %lu elements "
		       "but only got %lu", static_cast<long unsigned>(tot),
		       static_cast<long unsigned>(i));
	}
#else
	long ierr = selectValuesT(mask, *array);
	if (ierr < 0) {
	    LOGGER(ibis::gVerbose > 0)
		<< "Warning -- ibis::column["
		<< (thePart!=0 ? thePart->name() : "") << "." << m_name
		<< "]::selectValuesT failed with error code " << ierr;
	    delete array;
	    array = 0;
	}
#endif
    }
    else {
	logWarning("selectUBytes", "incompatible data type");
    }
    if (ibis::gVerbose > 4) {
	timer.stop();
	long unsigned cnt = mask.cnt();
	logMessage("selectUBytes", "retrieving %lu integer%s "
		   "took %g sec(CPU), %g sec(elapsed)",
		   static_cast<long unsigned>(cnt), (cnt > 1 ? "s" : ""),
		   timer.CPUTime(), timer.realTime());
    }
    return array;
} // ibis::column::selectUBytes

/// Can convert all integers 2-byte or less in length.  Note that unsigned
/// integers are simply treated as signed integers.  Shoter types
/// of signed integers are treated correctly as positive values.
/// @note The caller is responsible for freeing the returned array from any
/// of the selectTypes functions.
ibis::array_t<int16_t>*
ibis::column::selectShorts(const ibis::bitvector& mask) const {
    ibis::array_t<int16_t>* array = new array_t<int16_t>;
    const uint32_t tot = mask.cnt();
    if (tot == 0)
	return array;

    ibis::horometer timer;
    if (ibis::gVerbose > 4)
	timer.start();
    if (m_type == ibis::SHORT || m_type == ibis::USHORT) {
#if defined(FASTBIT_PREFER_READ_ALL)
	array_t<int16_t> prop;
	std::string sname;
	const char* fnm = dataFileName(sname);
	ibis::fileManager::ACCESS_PREFERENCE apref =
	    thePart->accessHint(mask, sizeof(int16_t));

	int ierr = ibis::fileManager::instance().getFile(fnm, prop, apref);
	if (ierr != 0) {
	    logWarning("selectShorts",
		       "the file manager faild to retrieve the content of"
		       " the data file \"%s\"", fnm);
	    return array;
	}

	uint32_t i = 0;
	array->resize(tot);
	const uint32_t nprop = prop.size();
	ibis::bitvector::indexSet index = mask.firstIndexSet();
	if (nprop >= mask.size()) { // no need to check loop bounds
	    while (index.nIndices() > 0) {
		const ibis::bitvector::word_t *idx0 = index.indices();
		if (index.isRange()) {
		    for (uint32_t j = *idx0; j < idx0[1]; ++j, ++i) {
			(*array)[i] = (prop[j]);
		    }
		}
		else {
		    for (uint32_t j = 0; j<index.nIndices(); ++j, ++i) {
			(*array)[i] = (prop[idx0[j]]);
		    }
		}
		++ index;
	    }
	}
	else { // need to check loop bounds against nprop
	    while (index.nIndices() > 0) {
		const ibis::bitvector::word_t *idx0 = index.indices();
		if (*idx0 >= nprop) break;
		if (index.isRange()) {
		    for (uint32_t j = *idx0;
			 j < (idx0[1]<=nprop ? idx0[1] : nprop);
			 ++j, ++i) {
			(*array)[i] = (prop[j]);
		    }
		}
		else {
		    for (uint32_t j = 0; j<index.nIndices(); ++j, ++i) {
			if (idx0[j] < nprop)
			    (*array)[i] = (prop[idx0[j]]);
			else
			    break;
		    }
		}
		++ index;
	    }
	}
	if (i != tot) {
	    array->resize(i);
	    logWarning("selectShorts", "expects to retrieve %lu elements "
		       "but only got %lu", static_cast<long unsigned>(tot),
		       static_cast<long unsigned>(i));
	}
#else
	long ierr = selectValuesT(mask, *array);
	if (ierr < 0) {
	    LOGGER(ibis::gVerbose > 0)
		<< "Warning -- ibis::column["
		<< (thePart!=0 ? thePart->name() : "") << "." << m_name
		<< "]::selectValuesT failed with error code " << ierr;
	    delete array;
	    array = 0;
	}
#endif
    }
    else if (m_type == ibis::BYTE) {
	array_t<char> prop;
	std::string sname;
	const char* fnm = dataFileName(sname);
	ibis::fileManager::ACCESS_PREFERENCE apref =
	    thePart != 0 ? thePart->accessHint(mask, sizeof(char))
	    : ibis::fileManager::MMAP_LARGE_FILES;

	int ierr = ibis::fileManager::instance().getFile(fnm, prop, apref);
	if (ierr != 0) {
	    logWarning("selectShorts",
		       "the file manager faild to retrieve the content of"
		       " the data file \"%s\"", fnm);
	    return array;
	}

	uint32_t i = 0;
	array->resize(tot);
	const uint32_t nprop = prop.size();
	ibis::bitvector::indexSet index = mask.firstIndexSet();
	if (nprop >= mask.size()) { // no need to check loop bounds
	    while (index.nIndices() > 0) {
		const ibis::bitvector::word_t *idx0 = index.indices();
		if (index.isRange()) {
		    for (uint32_t j = *idx0; j < idx0[1]; ++j, ++i) {
			(*array)[i] = (prop[j]);
		    }
		}
		else {
		    for (uint32_t j = 0; j<index.nIndices(); ++j, ++i) {
			(*array)[i] = (prop[idx0[j]]);
		    }
		}
		++ index;
	    }
	}
	else { // need to check loop bounds against nprop
	    while (index.nIndices() > 0) {
		const ibis::bitvector::word_t *idx0 = index.indices();
		if (*idx0 >= nprop) break;
		if (index.isRange()) {
		    for (uint32_t j = *idx0;
			 j < (idx0[1]<=nprop ? idx0[1] : nprop);
			 ++j, ++i) {
			(*array)[i] = (prop[j]);
		    }
		}
		else {
		    for (uint32_t j = 0; j<index.nIndices(); ++j, ++i) {
			if (idx0[j] < nprop)
			    (*array)[i] = (prop[idx0[j]]);
			else
			    break;
		    }
		}
		++ index;
	    }
	}
	if (i != tot) {
	    array->resize(i);
	    logWarning("selectShorts", "expects to retrieve %lu elements "
		       "but only got %lu", static_cast<long unsigned>(tot),
		       static_cast<long unsigned>(i));
	}
    }
    else if (m_type == ibis::UBYTE) {
	array_t<unsigned char> prop;
	std::string sname;
	const char* fnm = dataFileName(sname);
	ibis::fileManager::ACCESS_PREFERENCE apref =
	    thePart != 0 ? thePart->accessHint(mask, sizeof(char))
	    : ibis::fileManager::MMAP_LARGE_FILES;

	int ierr = ibis::fileManager::instance().getFile(fnm, prop, apref);
	if (ierr != 0) {
	    logWarning("selectShorts",
		       "the file manager faild to retrieve the content of"
		       " the data file \"%s\"", fnm);
	    return array;
	}

	uint32_t i = 0;
	array->resize(tot);
	const uint32_t nprop = prop.size();
	ibis::bitvector::indexSet index = mask.firstIndexSet();
	if (nprop >= mask.size()) { // no need to check loop bounds
	    while (index.nIndices() > 0) {
		const ibis::bitvector::word_t *idx0 = index.indices();
		if (index.isRange()) {
		    for (uint32_t j = *idx0; j < idx0[1]; ++j, ++i) {
			(*array)[i] = (prop[j]);
		    }
		}
		else {
		    for (uint32_t j = 0; j<index.nIndices(); ++j, ++i) {
			(*array)[i] = (prop[idx0[j]]);
		    }
		}
		++ index;
	    }
	}
	else { // need to check loop bounds against nprop
	    while (index.nIndices() > 0) {
		const ibis::bitvector::word_t *idx0 = index.indices();
		if (*idx0 >= nprop) break;
		if (index.isRange()) {
		    for (uint32_t j = *idx0;
			 j < (idx0[1]<=nprop ? idx0[1] : nprop);
			 ++j, ++i) {
			(*array)[i] = (prop[j]);
		    }
		}
		else {
		    for (uint32_t j = 0; j<index.nIndices(); ++j, ++i) {
			if (idx0[j] < nprop)
			    (*array)[i] = (prop[idx0[j]]);
			else
			    break;
		    }
		}
		++ index;
	    }
	}
	if (i != tot) {
	    array->resize(i);
	    logWarning("selectShorts", "expects to retrieve %lu elements "
		       "but only got %lu", static_cast<long unsigned>(tot),
		       static_cast<long unsigned>(i));
	}
    }
    else {
	logWarning("selectShorts", "incompatible data type");
    }
    if (ibis::gVerbose > 4) {
	timer.stop();
	long unsigned cnt = mask.cnt();
	logMessage("selectShorts", "retrieving %lu integer%s "
		   "took %g sec(CPU), %g sec(elapsed)",
		   static_cast<long unsigned>(cnt), (cnt > 1 ? "s" : ""),
		   timer.CPUTime(), timer.realTime());
    }
    return array;
} // ibis::column::selectShorts

/// @note The caller is responsible for freeing the returned array from any
/// of the selectTypes functions.
ibis::array_t<uint16_t>*
ibis::column::selectUShorts(const ibis::bitvector& mask) const {
    ibis::array_t<uint16_t>* array = new array_t<uint16_t>;
    const uint32_t tot = mask.cnt();
    if (tot == 0)
	return array;

    ibis::horometer timer;
    if (ibis::gVerbose > 4)
	timer.start();
    if (m_type == ibis::SHORT || m_type == ibis::USHORT) {
#if defined(FASTBIT_PREFER_READ_ALL)
	array_t<uint16_t> prop;
	std::string sname;
	const char* fnm = dataFileName(sname);
	ibis::fileManager::ACCESS_PREFERENCE apref =
	    thePart->accessHint(mask, sizeof(uint16_t));

	int ierr = ibis::fileManager::instance().getFile(fnm, prop, apref);
	if (ierr != 0) {
	    logWarning("selectUShorts",
		       "the file manager faild to retrieve the content of"
		       " the data file \"%s\"", fnm);
	    return array;
	}

	uint32_t i = 0;
	array->resize(tot);
	const uint32_t nprop = prop.size();
	ibis::bitvector::indexSet index = mask.firstIndexSet();
	if (nprop >= mask.size()) { // no need to check loop bounds
	    while (index.nIndices() > 0) {
		const ibis::bitvector::word_t *idx0 = index.indices();
		if (index.isRange()) {
		    for (uint32_t j = *idx0; j < idx0[1]; ++j, ++i) {
			(*array)[i] = (prop[j]);
		    }
		}
		else {
		    for (uint32_t j = 0; j<index.nIndices(); ++j, ++i) {
			(*array)[i] = (prop[idx0[j]]);
		    }
		}
		++ index;
	    }
	}
	else { // need to check loop bounds against nprop
	    while (index.nIndices() > 0) {
		const ibis::bitvector::word_t *idx0 = index.indices();
		if (*idx0 >= nprop) break;
		if (index.isRange()) {
		    for (uint32_t j = *idx0;
			 j < (idx0[1]<=nprop ? idx0[1] : nprop);
			 ++j, ++i) {
			(*array)[i] = (prop[j]);
		    }
		}
		else {
		    for (uint32_t j = 0; j<index.nIndices(); ++j, ++i) {
			if (idx0[j] < nprop)
			    (*array)[i] = (prop[idx0[j]]);
			else
			    break;
		    }
		}
		++ index;
	    }
	}
	if (i != tot) {
	    array->resize(i);
	    logWarning("selectUShorts", "expects to retrieve %lu elements "
		       "but only got %lu", static_cast<long unsigned>(tot),
		       static_cast<long unsigned>(i));
	}
#else
	long ierr = selectValuesT(mask, *array);
	if (ierr < 0) {
	    LOGGER(ibis::gVerbose > 0)
		<< "Warning -- ibis::column["
		<< (thePart!=0 ? thePart->name() : "") << "." << m_name
		<< "]::selectValuesT failed with error code " << ierr;
	    delete array;
	    array = 0;
	}
#endif
    }
    else if (m_type == ibis::BYTE) {
	array_t<char> prop;
	std::string sname;
	const char* fnm = dataFileName(sname);
	ibis::fileManager::ACCESS_PREFERENCE apref =
	    thePart != 0 ? thePart->accessHint(mask, sizeof(char))
	    : ibis::fileManager::MMAP_LARGE_FILES;

	int ierr = ibis::fileManager::instance().getFile(fnm, prop, apref);
	if (ierr != 0) {
	    logWarning("selectUShorts",
		       "the file manager faild to retrieve the content of"
		       " the data file \"%s\"", fnm);
	    return array;
	}

	uint32_t i = 0;
	array->resize(tot);
	const uint32_t nprop = prop.size();
	ibis::bitvector::indexSet index = mask.firstIndexSet();
	if (nprop >= mask.size()) { // no need to check loop bounds
	    while (index.nIndices() > 0) {
		const ibis::bitvector::word_t *idx0 = index.indices();
		if (index.isRange()) {
		    for (uint32_t j = *idx0; j < idx0[1]; ++j, ++i) {
			(*array)[i] = (prop[j]);
		    }
		}
		else {
		    for (uint32_t j = 0; j<index.nIndices(); ++j, ++i) {
			(*array)[i] = (prop[idx0[j]]);
		    }
		}
		++ index;
	    }
	}
	else { // need to check loop bounds against nprop
	    while (index.nIndices() > 0) {
		const ibis::bitvector::word_t *idx0 = index.indices();
		if (*idx0 >= nprop) break;
		if (index.isRange()) {
		    for (uint32_t j = *idx0;
			 j < (idx0[1]<=nprop ? idx0[1] : nprop);
			 ++j, ++i) {
			(*array)[i] = (prop[j]);
		    }
		}
		else {
		    for (uint32_t j = 0; j<index.nIndices(); ++j, ++i) {
			if (idx0[j] < nprop)
			    (*array)[i] = (prop[idx0[j]]);
			else
			    break;
		    }
		}
		++ index;
	    }
	}
	if (i != tot) {
	    array->resize(i);
	    logWarning("selectUShorts", "expects to retrieve %lu elements "
		       "but only got %lu", static_cast<long unsigned>(tot),
		       static_cast<long unsigned>(i));
	}
    }
    else if (m_type == ibis::UBYTE) {
	array_t<unsigned char> prop;
	std::string sname;
	const char* fnm = dataFileName(sname);
	ibis::fileManager::ACCESS_PREFERENCE apref =
	    thePart != 0 ? thePart->accessHint(mask, sizeof(char))
	    : ibis::fileManager::MMAP_LARGE_FILES;

	int ierr = ibis::fileManager::instance().getFile(fnm, prop, apref);
	if (ierr != 0) {
	    logWarning("selectUShorts",
		       "the file manager faild to retrieve the content of"
		       " the data file \"%s\"", fnm);
	    return array;
	}

	uint32_t i = 0;
	array->resize(tot);
	const uint32_t nprop = prop.size();
	ibis::bitvector::indexSet index = mask.firstIndexSet();
	if (nprop >= mask.size()) { // no need to check loop bounds
	    while (index.nIndices() > 0) {
		const ibis::bitvector::word_t *idx0 = index.indices();
		if (index.isRange()) {
		    for (uint32_t j = *idx0; j < idx0[1]; ++j, ++i) {
			(*array)[i] = (prop[j]);
		    }
		}
		else {
		    for (uint32_t j = 0; j<index.nIndices(); ++j, ++i) {
			(*array)[i] = (prop[idx0[j]]);
		    }
		}
		++ index;
	    }
	}
	else { // need to check loop bounds against nprop
	    while (index.nIndices() > 0) {
		const ibis::bitvector::word_t *idx0 = index.indices();
		if (*idx0 >= nprop) break;
		if (index.isRange()) {
		    for (uint32_t j = *idx0;
			 j < (idx0[1]<=nprop ? idx0[1] : nprop);
			 ++j, ++i) {
			(*array)[i] = (prop[j]);
		    }
		}
		else {
		    for (uint32_t j = 0; j<index.nIndices(); ++j, ++i) {
			if (idx0[j] < nprop)
			    (*array)[i] = (prop[idx0[j]]);
			else
			    break;
		    }
		}
		++ index;
	    }
	}
	if (i != tot) {
	    array->resize(i);
	    logWarning("selectUShorts", "expects to retrieve %lu elements "
		       "but only got %lu", static_cast<long unsigned>(tot),
		       static_cast<long unsigned>(i));
	}
    }
    else {
	logWarning("selectUShorts", "incompatible data type");
    }
    if (ibis::gVerbose > 4) {
	timer.stop();
	long unsigned cnt = mask.cnt();
	logMessage("selectUShorts", "retrieving %lu integer%s "
		   "took %g sec(CPU), %g sec(elapsed)",
		   static_cast<long unsigned>(cnt), (cnt > 1 ? "s" : ""),
		   timer.CPUTime(), timer.realTime());
    }
    return array;
} // ibis::column::selectUShorts

/// @note The caller is responsible for freeing the returned array from any
/// of the selectTypes functions.
ibis::array_t<int32_t>*
ibis::column::selectInts(const ibis::bitvector& mask) const {
    ibis::array_t<int32_t>* array = new array_t<int32_t>;
    const uint32_t tot = mask.cnt();
    if (tot == 0)
	return array;

    ibis::horometer timer;
    if (ibis::gVerbose > 4)
	timer.start();
    if (m_type == ibis::INT || m_type == ibis::UINT ||
	m_type == ibis::CATEGORY || m_type == ibis::TEXT) {
#if defined(FASTBIT_PREFER_READ_ALL)
	array_t<int32_t> prop;
	std::string sname;
	const char* fnm = dataFileName(sname);
	ibis::fileManager::ACCESS_PREFERENCE apref =
	    thePart != 0 ? thePart->accessHint(mask, sizeof(int32_t))
	    : ibis::fileManager::MMAP_LARGE_FILES;

	int ierr = ibis::fileManager::instance().getFile(fnm, prop, apref);
	if (ierr != 0) {
	    logWarning("selectInts",
		       "the file manager faild to retrieve the content of"
		       " the data file \"%s\"", fnm);
	    return array;
	}

	uint32_t i = 0;
	array->resize(tot);
	const long unsigned nprop = prop.size();
#if defined(DEBUG)
	logMessage("DEBUG", "selectInts mask.size(%lu) and nprop=%lu",
		   static_cast<long unsigned>(mask.size()), nprop);
#endif
	ibis::bitvector::indexSet index = mask.firstIndexSet();
	if (nprop >= mask.size()) { // no need to check loop bounds
#if defined(DEBUG)
	    logMessage("DEBUG", "entering unchecked loops");
#endif
	    while (index.nIndices() > 0) {
		const ibis::bitvector::word_t *idx0 = index.indices();
		if (index.isRange()) {
#if defined(DEBUG)
		    logMessage("DEBUG", "copying range [%lu, %lu), i=%lu",
			       static_cast<long unsigned>(*idx0),
			       static_cast<long unsigned>(idx0[1]),
			       static_cast<long unsigned>(i));
#endif
		    for (uint32_t j = *idx0; j < idx0[1]; ++j, ++i) {
			(*array)[i] = (prop[j]);
		    }
		}
		else {
		    for (uint32_t j = 0; j<index.nIndices(); ++j, ++i) {
#if defined(DEBUG)
			logMessage("DEBUG", "copying value %lu to i=%lu",
				   static_cast<long unsigned>(idx0[j]),
				   static_cast<long unsigned>(i));
#endif
			(*array)[i] = (prop[idx0[j]]);
		    }
		}
		++ index;
	    }
	}
	else { // need to check loop bounds against nprop
#if defined(DEBUG)
	    logMessage("DEBUG", "entering checked loops");
#endif
	    while (index.nIndices() > 0) {
		const ibis::bitvector::word_t *idx0 = index.indices();
		if (*idx0 >= nprop) break;
		if (index.isRange()) {
#if defined(DEBUG)
		    logMessage("DEBUG", "copying range [%lu, %lu), i=%lu",
			       static_cast<long unsigned>(*idx0),
			       static_cast<long unsigned>
			       (idx0[1]<=nprop ? idx0[1] : nprop),
			       static_cast<long unsigned>(i));
#endif
		    for (uint32_t j = *idx0;
			 j < (idx0[1]<=nprop ? idx0[1] : nprop);
			 ++j, ++i) {
			(*array)[i] = (prop[j]);
		    }
		}
		else {
		    for (uint32_t j = 0; j<index.nIndices(); ++j, ++i) {
#if defined(DEBUG)
			logMessage("DEBUG", "copying value %lu to i=%lu",
				   static_cast<long unsigned>(idx0[j]),
				   static_cast<long unsigned>(i));
#endif
			if (idx0[j] < nprop)
			    (*array)[i] = (prop[idx0[j]]);
			else
			    break;
		    }
		}
		++ index;
	    }
	}
	if (i != tot) {
	    array->resize(i);
	    logWarning("selectInts", "expects to retrieve %lu elements "
		       "but only got %lu", static_cast<long unsigned>(tot),
		       static_cast<long unsigned>(i));
	}
#else
	long ierr = selectValuesT(mask, *array);
	if (ierr < 0) {
	    LOGGER(ibis::gVerbose > 0)
		<< "Warning -- ibis::column["
		<< (thePart!=0 ? thePart->name() : "") << "." << m_name
		<< "]::selectValuesT failed with error code " << ierr;
	    delete array;
	    array = 0;
	}
#endif
    }
    else if (m_type == ibis::SHORT) {
	array_t<int16_t> prop;
	std::string sname;
	const char* fnm = dataFileName(sname);
	ibis::fileManager::ACCESS_PREFERENCE apref =
	    thePart->accessHint(mask, sizeof(int16_t));

	int ierr = ibis::fileManager::instance().getFile(fnm, prop, apref);
	if (ierr != 0) {
	    logWarning("selectInts",
		       "the file manager faild to retrieve the content of"
		       " the data file \"%s\"", fnm);
	    return array;
	}

	uint32_t i = 0;
	array->resize(tot);
	const uint32_t nprop = prop.size();
	ibis::bitvector::indexSet index = mask.firstIndexSet();
	if (nprop >= mask.size()) { // no need to check loop bounds
	    while (index.nIndices() > 0) {
		const ibis::bitvector::word_t *idx0 = index.indices();
		if (index.isRange()) {
		    for (uint32_t j = *idx0; j < idx0[1]; ++j, ++i) {
			(*array)[i] = (prop[j]);
		    }
		}
		else {
		    for (uint32_t j = 0; j<index.nIndices(); ++j, ++i) {
			(*array)[i] = (prop[idx0[j]]);
		    }
		}
		++ index;
	    }
	}
	else { // need to check loop bounds against nprop
	    while (index.nIndices() > 0) {
		const ibis::bitvector::word_t *idx0 = index.indices();
		if (*idx0 >= nprop) break;
		if (index.isRange()) {
		    for (uint32_t j = *idx0;
			 j < (idx0[1]<=nprop ? idx0[1] : nprop);
			 ++j, ++i) {
			(*array)[i] = (prop[j]);
		    }
		}
		else {
		    for (uint32_t j = 0; j<index.nIndices(); ++j, ++i) {
			if (idx0[j] < nprop)
			    (*array)[i] = (prop[idx0[j]]);
			else
			    break;
		    }
		}
		++ index;
	    }
	}
	if (i != tot) {
	    array->resize(i);
	    logWarning("selectInts", "expects to retrieve %lu elements "
		       "but only got %lu", static_cast<long unsigned>(tot),
		       static_cast<long unsigned>(i));
	}
    }
    else if (m_type == ibis::USHORT) {
	array_t<uint16_t> prop;
	std::string sname;
	const char* fnm = dataFileName(sname);
	ibis::fileManager::ACCESS_PREFERENCE apref =
	    thePart != 0 ? thePart->accessHint(mask, sizeof(uint16_t))
	    : ibis::fileManager::MMAP_LARGE_FILES;

	int ierr = ibis::fileManager::instance().getFile(fnm, prop, apref);
	if (ierr != 0) {
	    logWarning("selectInts",
		       "the file manager faild to retrieve the content of"
		       " the data file \"%s\"", fnm);
	    return array;
	}

	uint32_t i = 0;
	array->resize(tot);
	const uint32_t nprop = prop.size();
	ibis::bitvector::indexSet index = mask.firstIndexSet();
	if (nprop >= mask.size()) { // no need to check loop bounds
	    while (index.nIndices() > 0) {
		const ibis::bitvector::word_t *idx0 = index.indices();
		if (index.isRange()) {
		    for (uint32_t j = *idx0; j < idx0[1]; ++j, ++i) {
			(*array)[i] = (prop[j]);
		    }
		}
		else {
		    for (uint32_t j = 0; j<index.nIndices(); ++j, ++i) {
			(*array)[i] = (prop[idx0[j]]);
		    }
		}
		++ index;
	    }
	}
	else { // need to check loop bounds against nprop
	    while (index.nIndices() > 0) {
		const ibis::bitvector::word_t *idx0 = index.indices();
		if (*idx0 >= nprop) break;
		if (index.isRange()) {
		    for (uint32_t j = *idx0;
			 j < (idx0[1]<=nprop ? idx0[1] : nprop);
			 ++j, ++i) {
			(*array)[i] = (prop[j]);
		    }
		}
		else {
		    for (uint32_t j = 0; j<index.nIndices(); ++j, ++i) {
			if (idx0[j] < nprop)
			    (*array)[i] = (prop[idx0[j]]);
			else
			    break;
		    }
		}
		++ index;
	    }
	}
	if (i != tot) {
	    array->resize(i);
	    logWarning("selectInts", "expects to retrieve %lu elements "
		       "but only got %lu", static_cast<long unsigned>(tot),
		       static_cast<long unsigned>(i));
	}
    }
    else if (m_type == ibis::BYTE) {
	array_t<char> prop;
	std::string sname;
	const char* fnm = dataFileName(sname);
	ibis::fileManager::ACCESS_PREFERENCE apref =
	    thePart != 0 ? thePart->accessHint(mask, sizeof(char))
	    : ibis::fileManager::MMAP_LARGE_FILES;

	int ierr = ibis::fileManager::instance().getFile(fnm, prop, apref);
	if (ierr != 0) {
	    logWarning("selectInts",
		       "the file manager faild to retrieve the content of"
		       " the data file \"%s\"", fnm);
	    return array;
	}

	uint32_t i = 0;
	array->resize(tot);
	const uint32_t nprop = prop.size();
	ibis::bitvector::indexSet index = mask.firstIndexSet();
	if (nprop >= mask.size()) { // no need to check loop bounds
	    while (index.nIndices() > 0) {
		const ibis::bitvector::word_t *idx0 = index.indices();
		if (index.isRange()) {
		    for (uint32_t j = *idx0; j < idx0[1]; ++j, ++i) {
			(*array)[i] = (prop[j]);
		    }
		}
		else {
		    for (uint32_t j = 0; j<index.nIndices(); ++j, ++i) {
			(*array)[i] = (prop[idx0[j]]);
		    }
		}
		++ index;
	    }
	}
	else { // need to check loop bounds against nprop
	    while (index.nIndices() > 0) {
		const ibis::bitvector::word_t *idx0 = index.indices();
		if (*idx0 >= nprop) break;
		if (index.isRange()) {
		    for (uint32_t j = *idx0;
			 j < (idx0[1]<=nprop ? idx0[1] : nprop);
			 ++j, ++i) {
			(*array)[i] = (prop[j]);
		    }
		}
		else {
		    for (uint32_t j = 0; j<index.nIndices(); ++j, ++i) {
			if (idx0[j] < nprop)
			    (*array)[i] = (prop[idx0[j]]);
			else
			    break;
		    }
		}
		++ index;
	    }
	}
	if (i != tot) {
	    array->resize(i);
	    logWarning("selectInts", "expects to retrieve %lu elements "
		       "but only got %lu", static_cast<long unsigned>(tot),
		       static_cast<long unsigned>(i));
	}
    }
    else if (m_type == ibis::UBYTE) {
	array_t<unsigned char> prop;
	std::string sname;
	const char* fnm = dataFileName(sname);
	ibis::fileManager::ACCESS_PREFERENCE apref =
	    thePart != 0 ? thePart->accessHint(mask, sizeof(char))
	    : ibis::fileManager::MMAP_LARGE_FILES;

	int ierr = ibis::fileManager::instance().getFile(fnm, prop, apref);
	if (ierr != 0) {
	    logWarning("selectInts",
		       "the file manager faild to retrieve the content of"
		       " the data file \"%s\"", fnm);
	    return array;
	}

	uint32_t i = 0;
	array->resize(tot);
	const uint32_t nprop = prop.size();
	ibis::bitvector::indexSet index = mask.firstIndexSet();
	if (nprop >= mask.size()) { // no need to check loop bounds
	    while (index.nIndices() > 0) {
		const ibis::bitvector::word_t *idx0 = index.indices();
		if (index.isRange()) {
		    for (uint32_t j = *idx0; j < idx0[1]; ++j, ++i) {
			(*array)[i] = (prop[j]);
		    }
		}
		else {
		    for (uint32_t j = 0; j<index.nIndices(); ++j, ++i) {
			(*array)[i] = (prop[idx0[j]]);
		    }
		}
		++ index;
	    }
	}
	else { // need to check loop bounds against nprop
	    while (index.nIndices() > 0) {
		const ibis::bitvector::word_t *idx0 = index.indices();
		if (*idx0 >= nprop) break;
		if (index.isRange()) {
		    for (uint32_t j = *idx0;
			 j < (idx0[1]<=nprop ? idx0[1] : nprop);
			 ++j, ++i) {
			(*array)[i] = (prop[j]);
		    }
		}
		else {
		    for (uint32_t j = 0; j<index.nIndices(); ++j, ++i) {
			if (idx0[j] < nprop)
			    (*array)[i] = (prop[idx0[j]]);
			else
			    break;
		    }
		}
		++ index;
	    }
	}
	if (i != tot) {
	    array->resize(i);
	    logWarning("selectInts", "expects to retrieve %lu elements "
		       "but only got %lu", static_cast<long unsigned>(tot),
		       static_cast<long unsigned>(i));
	}
    }
    else {
	logWarning("selectInts", "incompatible data type");
    }
    if (ibis::gVerbose > 4) {
	timer.stop();
	long unsigned cnt = mask.cnt();
	logMessage("selectInts", "retrieving %lu integer%s "
		   "took %g sec(CPU), %g sec(elapsed)",
		   static_cast<long unsigned>(cnt), (cnt > 1 ? "s" : ""),
		   timer.CPUTime(), timer.realTime());
    }
    return array;
} // ibis::column::selectInts

/// Can be called on columns of unsigned integral types, UINT, CATEGORY,
/// USHORT, and UBYTE.
/// @note The caller is responsible for freeing the returned array from any
/// of the selectTypes functions.
ibis::array_t<uint32_t>*
ibis::column::selectUInts(const ibis::bitvector& mask) const {
    ibis::array_t<uint32_t>* array = new array_t<uint32_t>;
    const uint32_t tot = mask.cnt();
    if (tot == 0)
	return array;
    ibis::horometer timer;
    if (ibis::gVerbose > 4)
	timer.start();

    if (m_type == ibis::UINT || m_type == ibis::CATEGORY ||
	m_type == ibis::TEXT) {
#if defined(FASTBIT_PREFER_READ_ALL)
	array_t<uint32_t> prop;
	std::string sname;
	const char* fnm = dataFileName(sname);
	ibis::fileManager::ACCESS_PREFERENCE apref =
	    thePart != 0 ? thePart->accessHint(mask, sizeof(uint32_t))
	    : ibis::fileManager::MMAP_LARGE_FILES;

	int ierr = ibis::fileManager::instance().getFile(fnm, prop, apref);
	if (ierr != 0) {
	    logWarning("selectUInts",
		       "the file manager faild to retrieve the content of"
		       " the data file \"%s\"", fnm);
	    return array;
	}

	uint32_t i = 0;
	array->resize(tot);
	const uint32_t nprop = prop.size();
	ibis::bitvector::indexSet index = mask.firstIndexSet();
	if (nprop >= mask.size()) {
	    while (index.nIndices() > 0) {
		const ibis::bitvector::word_t *idx0 = index.indices();
		if (index.isRange()) {
		    for (uint32_t j = *idx0; j<idx0[1]; ++j, ++i) {
			(*array)[i] = (prop[j]);
		    }
		}
		else {
		    for (uint32_t j = 0; j<index.nIndices(); ++j, ++i) {
			(*array)[i] = (prop[idx0[j]]);
		    }
		}
		++ index;
	    }
	}
	else { // need to check loop bounds against nprop
	    while (index.nIndices() > 0) {
		const ibis::bitvector::word_t *idx0 = index.indices();
		if (*idx0 >= nprop) break;
		if (index.isRange()) {
		    for (uint32_t j = *idx0;
			 j < (idx0[1]<=nprop ? idx0[1] : nprop);
			 ++j, ++i) {
			(*array)[i] = (prop[j]);
		    }
		}
		else {
		    for (uint32_t j = 0; j<index.nIndices(); ++j, ++i) {
			if (idx0[j] < nprop)
			    (*array)[i] = (prop[idx0[j]]);
			else
			    break;
		    }
		}
		++ index;
	    }
	}

	if (i != tot) {
	    array->resize(i);
	    logWarning("selectUInts", "expects to retrieve %lu elements "
		       "but only got %lu", static_cast<long unsigned>(tot),
		       static_cast<long unsigned>(i));
	}
#else
	long ierr = selectValuesT(mask, *array);
	if (ierr < 0) {
	    LOGGER(ibis::gVerbose > 0)
		<< "Warning -- ibis::column["
		<< (thePart!=0 ? thePart->name() : "") << "." << m_name
		<< "]::selectValuesT failed with error code " << ierr;
	    delete array;
	    array = 0;
	}
#endif
    }
    else if (m_type == USHORT) {
	array_t<uint16_t> prop;
	std::string sname;
	const char* fnm = dataFileName(sname);
	ibis::fileManager::ACCESS_PREFERENCE apref =
	    thePart != 0 ? thePart->accessHint(mask, sizeof(uint16_t))
	    : ibis::fileManager::MMAP_LARGE_FILES;

	int ierr = ibis::fileManager::instance().getFile(fnm, prop, apref);
	if (ierr != 0) {
	    logWarning("selectUInts",
		       "the file manager faild to retrieve the content of"
		       " the data file \"%s\"", fnm);
	    return array;
	}

	uint32_t i = 0;
	array->resize(tot);
	const uint32_t nprop = prop.size();
	ibis::bitvector::indexSet index = mask.firstIndexSet();
	if (nprop >= mask.size()) {
	    while (index.nIndices() > 0) {
		const ibis::bitvector::word_t *idx0 = index.indices();
		if (index.isRange()) {
		    for (uint32_t j = *idx0; j<idx0[1]; ++j, ++i) {
			(*array)[i] = (prop[j]);
		    }
		}
		else {
		    for (uint32_t j = 0; j<index.nIndices(); ++j, ++i) {
			(*array)[i] = (prop[idx0[j]]);
		    }
		}
		++ index;
	    }
	}
	else { // need to check loop bounds against nprop
	    while (index.nIndices() > 0) {
		const ibis::bitvector::word_t *idx0 = index.indices();
		if (*idx0 >= nprop) break;
		if (index.isRange()) {
		    for (uint32_t j = *idx0;
			 j < (idx0[1]<=nprop ? idx0[1] : nprop);
			 ++j, ++i) {
			(*array)[i] = (prop[j]);
		    }
		}
		else {
		    for (uint32_t j = 0; j<index.nIndices(); ++j, ++i) {
			if (idx0[j] < nprop)
			    (*array)[i] = (prop[idx0[j]]);
			else
			    break;
		    }
		}
		++ index;
	    }
	}

	if (i != tot) {
	    array->resize(i);
	    logWarning("selectUInts", "expects to retrieve %lu elements "
		       "but only got %lu", static_cast<long unsigned>(tot),
		       static_cast<long unsigned>(i));
	}
    }
    else if (m_type == UBYTE) {
	array_t<unsigned char> prop;
	std::string sname;
	const char* fnm = dataFileName(sname);
	ibis::fileManager::ACCESS_PREFERENCE apref =
	    thePart != 0 ? thePart->accessHint(mask, sizeof(unsigned char))
	    : ibis::fileManager::MMAP_LARGE_FILES;

	int ierr = ibis::fileManager::instance().getFile(fnm, prop, apref);
	if (ierr != 0) {
	    logWarning("selectUInts",
		       "the file manager faild to retrieve the content of"
		       " the data file \"%s\"", fnm);
	    return array;
	}

	uint32_t i = 0;
	array->resize(tot);
	const uint32_t nprop = prop.size();
	ibis::bitvector::indexSet index = mask.firstIndexSet();
	if (nprop >= mask.size()) {
	    while (index.nIndices() > 0) {
		const ibis::bitvector::word_t *idx0 = index.indices();
		if (index.isRange()) {
		    for (uint32_t j = *idx0; j<idx0[1]; ++j, ++i) {
			(*array)[i] = (prop[j]);
		    }
		}
		else {
		    for (uint32_t j = 0; j<index.nIndices(); ++j, ++i) {
			(*array)[i] = (prop[idx0[j]]);
		    }
		}
		++ index;
	    }
	}
	else { // need to check loop bounds against nprop
	    while (index.nIndices() > 0) {
		const ibis::bitvector::word_t *idx0 = index.indices();
		if (*idx0 >= nprop) break;
		if (index.isRange()) {
		    for (uint32_t j = *idx0;
			 j < (idx0[1]<=nprop ? idx0[1] : nprop);
			 ++j, ++i) {
			(*array)[i] = (prop[j]);
		    }
		}
		else {
		    for (uint32_t j = 0; j<index.nIndices(); ++j, ++i) {
			if (idx0[j] < nprop)
			    (*array)[i] = (prop[idx0[j]]);
			else
			    break;
		    }
		}
		++ index;
	    }
	}

	if (i != tot) {
	    array->resize(i);
	    logWarning("selectUInts", "expects to retrieve %lu elements "
		       "but only got %lu", static_cast<long unsigned>(tot),
		       static_cast<long unsigned>(i));
	}
    }
    else {
	logWarning("selectUInts", "incompatible data type");
    }
    if (ibis::gVerbose > 4) {
	timer.stop();
	long unsigned cnt = mask.cnt();
	logMessage("selectUInts", "retrieving %lu unsigned integer%s "
		   "took %g sec(CPU), %g sec(elapsed)",
		   static_cast<long unsigned>(cnt), (cnt > 1 ? "s" : ""),
		   timer.CPUTime(), timer.realTime());
    }
    return array;
} // ibis::column::selectUInts

/// Can be called on all integral types.  Note that 64-byte unsigned
/// integers are simply treated as signed integers.  This may cause the
/// values to be interperted incorrectly.  Shorter version of unsigned
/// integers are treated correctly as positive values.
/// @note The caller is responsible for freeing the returned array from any
/// of the selectTypes functions.
ibis::array_t<int64_t>*
ibis::column::selectLongs(const ibis::bitvector& mask) const {
    array_t<int64_t>* array = new array_t<int64_t>;
    const uint32_t tot = mask.cnt();
    if (tot == 0)
	return array;

    ibis::horometer timer;
    if (ibis::gVerbose > 4)
	timer.start();
    if (m_type == ibis::LONG || m_type == ibis::ULONG) {
#if defined(FASTBIT_PREFER_READ_ALL)
	array_t<int64_t> prop;
	std::string sname;
	const char* fnm = dataFileName(sname);
	ibis::fileManager::ACCESS_PREFERENCE apref =
	    thePart != 0 ? thePart->accessHint(mask, sizeof(int64_t))
	    : ibis::fileManager::MMAP_LARGE_FILES;

	int ierr = ibis::fileManager::instance().getFile(fnm, prop, apref);
	if (ierr != 0) {
	    logWarning("selectLongs",
		       "the file manager faild to retrieve the content of"
		       " the data file \"%s\"", fnm);
	    return array;
	}

	uint32_t i = 0;
	array->resize(tot);
	const long unsigned nprop = prop.size();
#if defined(DEBUG)
	logMessage("DEBUG", "selectLongs mask.size(%lu) and nprop=%lu",
		   static_cast<long unsigned>(mask.size()), nprop);
#endif
	ibis::bitvector::indexSet index = mask.firstIndexSet();
	if (nprop >= mask.size()) { // no need to check loop bounds
#if defined(DEBUG)
	    logMessage("DEBUG", "entering unchecked loops");
#endif
	    while (index.nIndices() > 0) {
		const ibis::bitvector::word_t *idx0 = index.indices();
		if (index.isRange()) {
#if defined(DEBUG)
		    logMessage("DEBUG", "copying range [%lu, %lu), i=%lu",
			       static_cast<long unsigned>(*idx0),
			       static_cast<long unsigned>(idx0[1]),
			       static_cast<long unsigned>(i));
#endif
		    for (uint32_t j = *idx0; j < idx0[1]; ++j, ++i) {
			(*array)[i] = (prop[j]);
		    }
		}
		else {
		    for (uint32_t j = 0; j<index.nIndices(); ++j, ++i) {
#if defined(DEBUG)
			logMessage("DEBUG", "copying value %lu to i=%lu",
				   static_cast<long unsigned>(idx0[j]),
				   static_cast<long unsigned>(i));
#endif
			(*array)[i] = (prop[idx0[j]]);
		    }
		}
		++ index;
	    }
	}
	else { // need to check loop bounds against nprop
#if defined(DEBUG)
	    logMessage("DEBUG", "entering checked loops");
#endif
	    while (index.nIndices() > 0) {
		const ibis::bitvector::word_t *idx0 = index.indices();
		if (*idx0 >= nprop) break;
		if (index.isRange()) {
#if defined(DEBUG)
		    logMessage("DEBUG", "copying range [%lu, %lu), i=%lu",
			       static_cast<long unsigned>(*idx0),
			       static_cast<long unsigned>
			       (idx0[1]<=nprop ? idx0[1] : nprop),
			       static_cast<long unsigned>(i));
#endif
		    for (uint32_t j = *idx0;
			 j < (idx0[1]<=nprop ? idx0[1] : nprop);
			 ++j, ++i) {
			(*array)[i] = (prop[j]);
		    }
		}
		else {
		    for (uint32_t j = 0; j<index.nIndices(); ++j, ++i) {
#if defined(DEBUG)
			logMessage("DEBUG", "copying value %lu to i=%lu",
				   static_cast<long unsigned>(idx0[j]),
				   static_cast<long unsigned>(i));
#endif
			if (idx0[j] < nprop)
			    (*array)[i] = (prop[idx0[j]]);
			else
			    break;
		    }
		}
		++ index;
	    }
	}
	if (i != tot) {
	    array->resize(i);
	    logWarning("selectLongs", "expects to retrieve %lu elements "
		       "but only got %lu", static_cast<long unsigned>(tot),
		       static_cast<long unsigned>(i));
	}
#else
	long ierr = selectValuesT(mask, *array);
	if (ierr < 0) {
	    LOGGER(ibis::gVerbose > 0)
		<< "Warning -- ibis::column["
		<< (thePart!=0 ? thePart->name() : "") << "." << m_name
		<< "]::selectValuesT failed with error code " << ierr;
	    delete array;
	    array = 0;
	}
#endif
    }
    else if (m_type == ibis::UINT || m_type == ibis::CATEGORY ||
	     m_type == ibis::TEXT) {
	array_t<uint32_t> prop;
	std::string sname;
	const char* fnm = dataFileName(sname);
	ibis::fileManager::ACCESS_PREFERENCE apref =
	    thePart != 0 ? thePart->accessHint(mask, sizeof(uint32_t))
	    : ibis::fileManager::MMAP_LARGE_FILES;

	int ierr = ibis::fileManager::instance().getFile(fnm, prop, apref);
	if (ierr != 0) {
	    logWarning("selectLongs",
		       "the file manager faild to retrieve the content of"
		       " the data file \"%s\"", fnm);
	    return array;
	}

	uint32_t i = 0;
	array->resize(tot);
	const long unsigned nprop = prop.size();
#if defined(DEBUG)
	logMessage("DEBUG", "selectLongs mask.size(%lu) and nprop=%lu",
		   static_cast<long unsigned>(mask.size()), nprop);
#endif
	ibis::bitvector::indexSet index = mask.firstIndexSet();
	if (nprop >= mask.size()) { // no need to check loop bounds
#if defined(DEBUG)
	    logMessage("DEBUG", "entering unchecked loops");
#endif
	    while (index.nIndices() > 0) {
		const ibis::bitvector::word_t *idx0 = index.indices();
		if (index.isRange()) {
#if defined(DEBUG)
		    logMessage("DEBUG", "copying range [%lu, %lu), i=%lu",
			       static_cast<long unsigned>(*idx0),
			       static_cast<long unsigned>(idx0[1]),
			       static_cast<long unsigned>(i));
#endif
		    for (uint32_t j = *idx0; j < idx0[1]; ++j, ++i) {
			(*array)[i] = (prop[j]);
		    }
		}
		else {
		    for (uint32_t j = 0; j<index.nIndices(); ++j, ++i) {
#if defined(DEBUG)
			logMessage("DEBUG", "copying value %lu to i=%lu",
				   static_cast<long unsigned>(idx0[j]),
				   static_cast<long unsigned>(i));
#endif
			(*array)[i] = (prop[idx0[j]]);
		    }
		}
		++ index;
	    }
	}
	else { // need to check loop bounds against nprop
#if defined(DEBUG)
	    logMessage("DEBUG", "entering checked loops");
#endif
	    while (index.nIndices() > 0) {
		const ibis::bitvector::word_t *idx0 = index.indices();
		if (*idx0 >= nprop) break;
		if (index.isRange()) {
#if defined(DEBUG)
		    logMessage("DEBUG", "copying range [%lu, %lu), i=%lu",
			       static_cast<long unsigned>(*idx0),
			       static_cast<long unsigned>
			       (idx0[1]<=nprop ? idx0[1] : nprop),
			       static_cast<long unsigned>(i));
#endif
		    for (uint32_t j = *idx0;
			 j < (idx0[1]<=nprop ? idx0[1] : nprop);
			 ++j, ++i) {
			(*array)[i] = (prop[j]);
		    }
		}
		else {
		    for (uint32_t j = 0; j<index.nIndices(); ++j, ++i) {
#if defined(DEBUG)
			logMessage("DEBUG", "copying value %lu to i=%lu",
				   static_cast<long unsigned>(idx0[j]),
				   static_cast<long unsigned>(i));
#endif
			if (idx0[j] < nprop)
			    (*array)[i] = (prop[idx0[j]]);
			else
			    break;
		    }
		}
		++ index;
	    }
	}
	if (i != tot) {
	    array->resize(i);
	    logWarning("selectLongs", "expects to retrieve %lu elements "
		       "but only got %lu", static_cast<long unsigned>(tot),
		       static_cast<long unsigned>(i));
	}
    }
    else if (m_type == ibis::INT) {
	array_t<int32_t> prop;
	std::string sname;
	const char* fnm = dataFileName(sname);
	ibis::fileManager::ACCESS_PREFERENCE apref =
	    thePart != 0 ? thePart->accessHint(mask, sizeof(int32_t))
	    : ibis::fileManager::MMAP_LARGE_FILES;

	int ierr = ibis::fileManager::instance().getFile(fnm, prop, apref);
	if (ierr != 0) {
	    logWarning("selectLongs",
		       "the file manager faild to retrieve the content of"
		       " the data file \"%s\"", fnm);
	    return array;
	}

	uint32_t i = 0;
	array->resize(tot);
	const long unsigned nprop = prop.size();
#if defined(DEBUG)
	logMessage("DEBUG", "selectLongs mask.size(%lu) and nprop=%lu",
		   static_cast<long unsigned>(mask.size()), nprop);
#endif
	ibis::bitvector::indexSet index = mask.firstIndexSet();
	if (nprop >= mask.size()) { // no need to check loop bounds
#if defined(DEBUG)
	    logMessage("DEBUG", "entering unchecked loops");
#endif
	    while (index.nIndices() > 0) {
		const ibis::bitvector::word_t *idx0 = index.indices();
		if (index.isRange()) {
#if defined(DEBUG)
		    logMessage("DEBUG", "copying range [%lu, %lu), i=%lu",
			       static_cast<long unsigned>(*idx0),
			       static_cast<long unsigned>(idx0[1]),
			       static_cast<long unsigned>(i));
#endif
		    for (uint32_t j = *idx0; j < idx0[1]; ++j, ++i) {
			(*array)[i] = (prop[j]);
		    }
		}
		else {
		    for (uint32_t j = 0; j<index.nIndices(); ++j, ++i) {
#if defined(DEBUG)
			logMessage("DEBUG", "copying value %lu to i=%lu",
				   static_cast<long unsigned>(idx0[j]),
				   static_cast<long unsigned>(i));
#endif
			(*array)[i] = (prop[idx0[j]]);
		    }
		}
		++ index;
	    }
	}
	else { // need to check loop bounds against nprop
#if defined(DEBUG)
	    logMessage("DEBUG", "entering checked loops");
#endif
	    while (index.nIndices() > 0) {
		const ibis::bitvector::word_t *idx0 = index.indices();
		if (*idx0 >= nprop) break;
		if (index.isRange()) {
#if defined(DEBUG)
		    logMessage("DEBUG", "copying range [%lu, %lu), i=%lu",
			       static_cast<long unsigned>(*idx0),
			       static_cast<long unsigned>
			       (idx0[1]<=nprop ? idx0[1] : nprop),
			       static_cast<long unsigned>(i));
#endif
		    for (uint32_t j = *idx0;
			 j < (idx0[1]<=nprop ? idx0[1] : nprop);
			 ++j, ++i) {
			(*array)[i] = (prop[j]);
		    }
		}
		else {
		    for (uint32_t j = 0; j<index.nIndices(); ++j, ++i) {
#if defined(DEBUG)
			logMessage("DEBUG", "copying value %lu to i=%lu",
				   static_cast<long unsigned>(idx0[j]),
				   static_cast<long unsigned>(i));
#endif
			if (idx0[j] < nprop)
			    (*array)[i] = (prop[idx0[j]]);
			else
			    break;
		    }
		}
		++ index;
	    }
	}
	if (i != tot) {
	    array->resize(i);
	    logWarning("selectLongs", "expects to retrieve %lu elements "
		       "but only got %lu", static_cast<long unsigned>(tot),
		       static_cast<long unsigned>(i));
	}
    }
    else if (m_type == ibis::USHORT) {
	array_t<uint16_t> prop;
	std::string sname;
	const char* fnm = dataFileName(sname);
	ibis::fileManager::ACCESS_PREFERENCE apref =
	    thePart != 0 ? thePart->accessHint(mask, sizeof(uint16_t))
	    : ibis::fileManager::MMAP_LARGE_FILES;

	int ierr = ibis::fileManager::instance().getFile(fnm, prop, apref);
	if (ierr != 0) {
	    logWarning("selectLongs",
		       "the file manager faild to retrieve the content of"
		       " the data file \"%s\"", fnm);
	    return array;
	}

	uint32_t i = 0;
	array->resize(tot);
	const uint32_t nprop = prop.size();
	ibis::bitvector::indexSet index = mask.firstIndexSet();
	if (nprop >= mask.size()) { // no need to check loop bounds
	    while (index.nIndices() > 0) {
		const ibis::bitvector::word_t *idx0 = index.indices();
		if (index.isRange()) {
		    for (uint32_t j = *idx0; j < idx0[1]; ++j, ++i) {
			(*array)[i] = (prop[j]);
		    }
		}
		else {
		    for (uint32_t j = 0; j<index.nIndices(); ++j, ++i) {
			(*array)[i] = (prop[idx0[j]]);
		    }
		}
		++ index;
	    }
	}
	else { // need to check loop bounds against nprop
	    while (index.nIndices() > 0) {
		const ibis::bitvector::word_t *idx0 = index.indices();
		if (*idx0 >= nprop) break;
		if (index.isRange()) {
		    for (uint32_t j = *idx0;
			 j < (idx0[1]<=nprop ? idx0[1] : nprop);
			 ++j, ++i) {
			(*array)[i] = (prop[j]);
		    }
		}
		else {
		    for (uint32_t j = 0; j<index.nIndices(); ++j, ++i) {
			if (idx0[j] < nprop)
			    (*array)[i] = (prop[idx0[j]]);
			else
			    break;
		    }
		}
		++ index;
	    }
	}
	if (i != tot) {
	    array->resize(i);
	    logWarning("selectLongs", "expects to retrieve %lu elements "
		       "but only got %lu", static_cast<long unsigned>(tot),
		       static_cast<long unsigned>(i));
	}
    }
    else if (m_type == SHORT) {
	array_t<int16_t> prop;
	std::string sname;
	const char* fnm = dataFileName(sname);
	ibis::fileManager::ACCESS_PREFERENCE apref =
	    thePart != 0 ? thePart->accessHint(mask, sizeof(int16_t))
	    : ibis::fileManager::MMAP_LARGE_FILES;

	int ierr = ibis::fileManager::instance().getFile(fnm, prop, apref);
	if (ierr != 0) {
	    logWarning("selectLongs",
		       "the file manager faild to retrieve the content of"
		       " the data file \"%s\"", fnm);
	    return array;
	}

	uint32_t i = 0;
	array->resize(tot);
	const uint32_t nprop = prop.size();
	ibis::bitvector::indexSet index = mask.firstIndexSet();
	if (nprop >= mask.size()) { // no need to check loop bounds
	    while (index.nIndices() > 0) {
		const ibis::bitvector::word_t *idx0 = index.indices();
		if (index.isRange()) {
		    for (uint32_t j = *idx0; j < idx0[1]; ++j, ++i) {
			(*array)[i] = (prop[j]);
		    }
		}
		else {
		    for (uint32_t j = 0; j<index.nIndices(); ++j, ++i) {
			(*array)[i] = (prop[idx0[j]]);
		    }
		}
		++ index;
	    }
	}
	else { // need to check loop bounds against nprop
	    while (index.nIndices() > 0) {
		const ibis::bitvector::word_t *idx0 = index.indices();
		if (*idx0 >= nprop) break;
		if (index.isRange()) {
		    for (uint32_t j = *idx0;
			 j < (idx0[1]<=nprop ? idx0[1] : nprop);
			 ++j, ++i) {
			(*array)[i] = (prop[j]);
		    }
		}
		else {
		    for (uint32_t j = 0; j<index.nIndices(); ++j, ++i) {
			if (idx0[j] < nprop)
			    (*array)[i] = (prop[idx0[j]]);
			else
			    break;
		    }
		}
		++ index;
	    }
	}
	if (i != tot) {
	    array->resize(i);
	    logWarning("selectLongs", "expects to retrieve %lu elements "
		       "but only got %lu", static_cast<long unsigned>(tot),
		       static_cast<long unsigned>(i));
	}
    }
    else if (m_type == UBYTE) {
	array_t<unsigned char> prop;
	std::string sname;
	const char* fnm = dataFileName(sname);
	ibis::fileManager::ACCESS_PREFERENCE apref =
	    thePart != 0 ? thePart->accessHint(mask, sizeof(unsigned char))
	    : ibis::fileManager::MMAP_LARGE_FILES;

	int ierr = ibis::fileManager::instance().getFile(fnm, prop, apref);
	if (ierr != 0) {
	    logWarning("selectLongs",
		       "the file manager faild to retrieve the content of"
		       " the data file \"%s\"", fnm);
	    return array;
	}

	uint32_t i = 0;
	array->resize(tot);
	const uint32_t nprop = prop.size();
	ibis::bitvector::indexSet index = mask.firstIndexSet();
	if (nprop >= mask.size()) { // no need to check loop bounds
	    while (index.nIndices() > 0) {
		const ibis::bitvector::word_t *idx0 = index.indices();
		if (index.isRange()) {
		    for (uint32_t j = *idx0; j < idx0[1]; ++j, ++i) {
			(*array)[i] = (prop[j]);
		    }
		}
		else {
		    for (uint32_t j = 0; j<index.nIndices(); ++j, ++i) {
			(*array)[i] = (prop[idx0[j]]);
		    }
		}
		++ index;
	    }
	}
	else { // need to check loop bounds against nprop
	    while (index.nIndices() > 0) {
		const ibis::bitvector::word_t *idx0 = index.indices();
		if (*idx0 >= nprop) break;
		if (index.isRange()) {
		    for (uint32_t j = *idx0;
			 j < (idx0[1]<=nprop ? idx0[1] : nprop);
			 ++j, ++i) {
			(*array)[i] = (prop[j]);
		    }
		}
		else {
		    for (uint32_t j = 0; j<index.nIndices(); ++j, ++i) {
			if (idx0[j] < nprop)
			    (*array)[i] = (prop[idx0[j]]);
			else
			    break;
		    }
		}
		++ index;
	    }
	}
	if (i != tot) {
	    array->resize(i);
	    logWarning("selectLongs", "expects to retrieve %lu elements "
		       "but only got %lu", static_cast<long unsigned>(tot),
		       static_cast<long unsigned>(i));
	}
    }
    else if (m_type == BYTE) {
	array_t<char> prop;
	std::string sname;
	const char* fnm = dataFileName(sname);
	ibis::fileManager::ACCESS_PREFERENCE apref =
	    thePart != 0 ? thePart->accessHint(mask, sizeof(char))
	    : ibis::fileManager::MMAP_LARGE_FILES;

	int ierr = ibis::fileManager::instance().getFile(fnm, prop, apref);
	if (ierr != 0) {
	    logWarning("selectLongs",
		       "the file manager faild to retrieve the content of"
		       " the data file \"%s\"", fnm);
	    return array;
	}

	uint32_t i = 0;
	array->resize(tot);
	const uint32_t nprop = prop.size();
	ibis::bitvector::indexSet index = mask.firstIndexSet();
	if (nprop >= mask.size()) { // no need to check loop bounds
	    while (index.nIndices() > 0) {
		const ibis::bitvector::word_t *idx0 = index.indices();
		if (index.isRange()) {
		    for (uint32_t j = *idx0; j < idx0[1]; ++j, ++i) {
			(*array)[i] = (prop[j]);
		    }
		}
		else {
		    for (uint32_t j = 0; j<index.nIndices(); ++j, ++i) {
			(*array)[i] = (prop[idx0[j]]);
		    }
		}
		++ index;
	    }
	}
	else { // need to check loop bounds against nprop
	    while (index.nIndices() > 0) {
		const ibis::bitvector::word_t *idx0 = index.indices();
		if (*idx0 >= nprop) break;
		if (index.isRange()) {
		    for (uint32_t j = *idx0;
			 j < (idx0[1]<=nprop ? idx0[1] : nprop);
			 ++j, ++i) {
			(*array)[i] = (prop[j]);
		    }
		}
		else {
		    for (uint32_t j = 0; j<index.nIndices(); ++j, ++i) {
			if (idx0[j] < nprop)
			    (*array)[i] = (prop[idx0[j]]);
			else
			    break;
		    }
		}
		++ index;
	    }
	}
	if (i != tot) {
	    array->resize(i);
	    logWarning("selectLongs", "expects to retrieve %lu elements "
		       "but only got %lu", static_cast<long unsigned>(tot),
		       static_cast<long unsigned>(i));
	}
    }
    else {
	logWarning("selectLongs", "incompatible data type");
    }
    if (ibis::gVerbose > 4) {
	timer.stop();
	long unsigned cnt = mask.cnt();
	logMessage("selectLongs", "retrieving %lu integer%s "
		   "took %g sec(CPU), %g sec(elapsed)",
		   static_cast<long unsigned>(cnt), (cnt > 1 ? "s" : ""),
		   timer.CPUTime(), timer.realTime());
    }
    return array;
} // ibis::column::selectLongs

/// Can be called on all unsigned integral types.
/// @note The caller is responsible for freeing the returned array from any
/// of the selectTypes functions.
ibis::array_t<uint64_t>*
ibis::column::selectULongs(const ibis::bitvector& mask) const {
    ibis::array_t<uint64_t>* array = new array_t<uint64_t>;
    const uint32_t tot = mask.cnt();
    if (tot == 0)
	return array;

    ibis::horometer timer;
    if (ibis::gVerbose > 4)
	timer.start();
    if (m_type == ibis::ULONG) {
#if defined(FASTBIT_PREFER_READ_ALL)
	array_t<uint64_t> prop;
	std::string sname;
	const char* fnm = dataFileName(sname);
	ibis::fileManager::ACCESS_PREFERENCE apref =
	    thePart != 0 ? thePart->accessHint(mask, sizeof(uint64_t))
	    : ibis::fileManager::MMAP_LARGE_FILES;

	int ierr = ibis::fileManager::instance().getFile(fnm, prop, apref);
	if (ierr != 0) {
	    logWarning("selectULongs",
		       "the file manager faild to retrieve the content of"
		       " the data file \"%s\"", fnm);
	    return array;
	}

	uint32_t i = 0;
	array->resize(tot);
	const long unsigned nprop = prop.size();
#if defined(DEBUG)
	logMessage("DEBUG", "selectULongs mask.size(%lu) and nprop=%lu",
		   static_cast<long unsigned>(mask.size()), nprop);
#endif
	ibis::bitvector::indexSet index = mask.firstIndexSet();
	if (nprop >= mask.size()) { // no need to check loop bounds
#if defined(DEBUG)
	    logMessage("DEBUG", "entering unchecked loops");
#endif
	    while (index.nIndices() > 0) {
		const ibis::bitvector::word_t *idx0 = index.indices();
		if (index.isRange()) {
#if defined(DEBUG)
		    logMessage("DEBUG", "copying range [%lu, %lu), i=%lu",
			       static_cast<long unsigned>(*idx0),
			       static_cast<long unsigned>(idx0[1]),
			       static_cast<long unsigned>(i));
#endif
		    for (uint32_t j = *idx0; j < idx0[1]; ++j, ++i) {
			(*array)[i] = (prop[j]);
		    }
		}
		else {
		    for (uint32_t j = 0; j<index.nIndices(); ++j, ++i) {
#if defined(DEBUG)
			logMessage("DEBUG", "copying value %lu to i=%lu",
				   static_cast<long unsigned>(idx0[j]),
				   static_cast<long unsigned>(i));
#endif
			(*array)[i] = (prop[idx0[j]]);
		    }
		}
		++ index;
	    }
	}
	else { // need to check loop bounds against nprop
#if defined(DEBUG)
	    logMessage("DEBUG", "entering checked loops");
#endif
	    while (index.nIndices() > 0) {
		const ibis::bitvector::word_t *idx0 = index.indices();
		if (*idx0 >= nprop) break;
		if (index.isRange()) {
#if defined(DEBUG)
		    logMessage("DEBUG", "copying range [%lu, %lu), i=%lu",
			       static_cast<long unsigned>(*idx0),
			       static_cast<long unsigned>
			       (idx0[1]<=nprop ? idx0[1] : nprop),
			       static_cast<long unsigned>(i));
#endif
		    for (uint32_t j = *idx0;
			 j < (idx0[1]<=nprop ? idx0[1] : nprop);
			 ++j, ++i) {
			(*array)[i] = (prop[j]);
		    }
		}
		else {
		    for (uint32_t j = 0; j<index.nIndices(); ++j, ++i) {
#if defined(DEBUG)
			logMessage("DEBUG", "copying value %lu to i=%lu",
				   static_cast<long unsigned>(idx0[j]),
				   static_cast<long unsigned>(i));
#endif
			if (idx0[j] < nprop)
			    (*array)[i] = (prop[idx0[j]]);
			else
			    break;
		    }
		}
		++ index;
	    }
	}
	if (i != tot) {
	    array->resize(i);
	    logWarning("selectULongs", "expects to retrieve %lu elements "
		       "but only got %lu", static_cast<long unsigned>(tot),
		       static_cast<long unsigned>(i));
	}
#else
	long ierr = selectValuesT(mask, *array);
	if (ierr < 0) {
	    LOGGER(ibis::gVerbose > 0)
		<< "Warning -- ibis::column["
		<< (thePart!=0 ? thePart->name() : "") << "." << m_name
		<< "]::selectValuesT failed with error code " << ierr;
	    delete array;
	    array = 0;
	}
#endif
    }
    else if (m_type == ibis::UINT || m_type == ibis::CATEGORY ||
	     m_type == ibis::TEXT) {
	array_t<uint32_t> prop;
	std::string sname;
	const char* fnm = dataFileName(sname);
	ibis::fileManager::ACCESS_PREFERENCE apref =
	    thePart != 0 ? thePart->accessHint(mask, sizeof(uint32_t))
	    : ibis::fileManager::MMAP_LARGE_FILES;

	int ierr = ibis::fileManager::instance().getFile(fnm, prop, apref);
	if (ierr != 0) {
	    logWarning("selectULongs",
		       "the file manager faild to retrieve the content of"
		       " the data file \"%s\"", fnm);
	    return array;
	}

	uint32_t i = 0;
	array->resize(tot);
	const long unsigned nprop = prop.size();
#if defined(DEBUG)
	logMessage("DEBUG", "selectULongs mask.size(%lu) and nprop=%lu",
		   static_cast<long unsigned>(mask.size()), nprop);
#endif
	ibis::bitvector::indexSet index = mask.firstIndexSet();
	if (nprop >= mask.size()) { // no need to check loop bounds
#if defined(DEBUG)
	    logMessage("DEBUG", "entering unchecked loops");
#endif
	    while (index.nIndices() > 0) {
		const ibis::bitvector::word_t *idx0 = index.indices();
		if (index.isRange()) {
#if defined(DEBUG)
		    logMessage("DEBUG", "copying range [%lu, %lu), i=%lu",
			       static_cast<long unsigned>(*idx0),
			       static_cast<long unsigned>(idx0[1]),
			       static_cast<long unsigned>(i));
#endif
		    for (uint32_t j = *idx0; j < idx0[1]; ++j, ++i) {
			(*array)[i] = (prop[j]);
		    }
		}
		else {
		    for (uint32_t j = 0; j<index.nIndices(); ++j, ++i) {
#if defined(DEBUG)
			logMessage("DEBUG", "copying value %lu to i=%lu",
				   static_cast<long unsigned>(idx0[j]),
				   static_cast<long unsigned>(i));
#endif
			(*array)[i] = (prop[idx0[j]]);
		    }
		}
		++ index;
	    }
	}
	else { // need to check loop bounds against nprop
#if defined(DEBUG)
	    logMessage("DEBUG", "entering checked loops");
#endif
	    while (index.nIndices() > 0) {
		const ibis::bitvector::word_t *idx0 = index.indices();
		if (*idx0 >= nprop) break;
		if (index.isRange()) {
#if defined(DEBUG)
		    logMessage("DEBUG", "copying range [%lu, %lu), i=%lu",
			       static_cast<long unsigned>(*idx0),
			       static_cast<long unsigned>
			       (idx0[1]<=nprop ? idx0[1] : nprop),
			       static_cast<long unsigned>(i));
#endif
		    for (uint32_t j = *idx0;
			 j < (idx0[1]<=nprop ? idx0[1] : nprop);
			 ++j, ++i) {
			(*array)[i] = (prop[j]);
		    }
		}
		else {
		    for (uint32_t j = 0; j<index.nIndices(); ++j, ++i) {
#if defined(DEBUG)
			logMessage("DEBUG", "copying value %lu to i=%lu",
				   static_cast<long unsigned>(idx0[j]),
				   static_cast<long unsigned>(i));
#endif
			if (idx0[j] < nprop)
			    (*array)[i] = (prop[idx0[j]]);
			else
			    break;
		    }
		}
		++ index;
	    }
	}
	if (i != tot) {
	    array->resize(i);
	    logWarning("selectULongs", "expects to retrieve %lu elements "
		       "but only got %lu", static_cast<long unsigned>(tot),
		       static_cast<long unsigned>(i));
	}
    }
    else if (m_type == ibis::USHORT) {
	array_t<uint16_t> prop;
	std::string sname;
	const char* fnm = dataFileName(sname);
	ibis::fileManager::ACCESS_PREFERENCE apref =
	    thePart != 0 ? thePart->accessHint(mask, sizeof(uint16_t))
	    : ibis::fileManager::MMAP_LARGE_FILES;

	int ierr = ibis::fileManager::instance().getFile(fnm, prop, apref);
	if (ierr != 0) {
	    logWarning("selectULongs",
		       "the file manager faild to retrieve the content of"
		       " the data file \"%s\"", fnm);
	    return array;
	}

	uint32_t i = 0;
	array->resize(tot);
	const uint32_t nprop = prop.size();
	ibis::bitvector::indexSet index = mask.firstIndexSet();
	if (nprop >= mask.size()) { // no need to check loop bounds
	    while (index.nIndices() > 0) {
		const ibis::bitvector::word_t *idx0 = index.indices();
		if (index.isRange()) {
		    for (uint32_t j = *idx0; j < idx0[1]; ++j, ++i) {
			(*array)[i] = (prop[j]);
		    }
		}
		else {
		    for (uint32_t j = 0; j<index.nIndices(); ++j, ++i) {
			(*array)[i] = (prop[idx0[j]]);
		    }
		}
		++ index;
	    }
	}
	else { // need to check loop bounds against nprop
	    while (index.nIndices() > 0) {
		const ibis::bitvector::word_t *idx0 = index.indices();
		if (*idx0 >= nprop) break;
		if (index.isRange()) {
		    for (uint32_t j = *idx0;
			 j < (idx0[1]<=nprop ? idx0[1] : nprop);
			 ++j, ++i) {
			(*array)[i] = (prop[j]);
		    }
		}
		else {
		    for (uint32_t j = 0; j<index.nIndices(); ++j, ++i) {
			if (idx0[j] < nprop)
			    (*array)[i] = (prop[idx0[j]]);
			else
			    break;
		    }
		}
		++ index;
	    }
	}
	if (i != tot) {
	    array->resize(i);
	    logWarning("selectULongs", "expects to retrieve %lu elements "
		       "but only got %lu", static_cast<long unsigned>(tot),
		       static_cast<long unsigned>(i));
	}
    }
    else if (m_type == UBYTE) {
	array_t<unsigned char> prop;
	std::string sname;
	const char* fnm = dataFileName(sname);
	ibis::fileManager::ACCESS_PREFERENCE apref =
	    thePart != 0 ? thePart->accessHint(mask, sizeof(unsigned char))
	    : ibis::fileManager::MMAP_LARGE_FILES;

	int ierr = ibis::fileManager::instance().getFile(fnm, prop, apref);
	if (ierr != 0) {
	    logWarning("selectULongs",
		       "the file manager faild to retrieve the content of"
		       " the data file \"%s\"", fnm);
	    return array;
	}

	uint32_t i = 0;
	array->resize(tot);
	const uint32_t nprop = prop.size();
	ibis::bitvector::indexSet index = mask.firstIndexSet();
	if (nprop >= mask.size()) { // no need to check loop bounds
	    while (index.nIndices() > 0) {
		const ibis::bitvector::word_t *idx0 = index.indices();
		if (index.isRange()) {
		    for (uint32_t j = *idx0; j < idx0[1]; ++j, ++i) {
			(*array)[i] = (prop[j]);
		    }
		}
		else {
		    for (uint32_t j = 0; j<index.nIndices(); ++j, ++i) {
			(*array)[i] = (prop[idx0[j]]);
		    }
		}
		++ index;
	    }
	}
	else { // need to check loop bounds against nprop
	    while (index.nIndices() > 0) {
		const ibis::bitvector::word_t *idx0 = index.indices();
		if (*idx0 >= nprop) break;
		if (index.isRange()) {
		    for (uint32_t j = *idx0;
			 j < (idx0[1]<=nprop ? idx0[1] : nprop);
			 ++j, ++i) {
			(*array)[i] = (prop[j]);
		    }
		}
		else {
		    for (uint32_t j = 0; j<index.nIndices(); ++j, ++i) {
			if (idx0[j] < nprop)
			    (*array)[i] = (prop[idx0[j]]);
			else
			    break;
		    }
		}
		++ index;
	    }
	}
	if (i != tot) {
	    array->resize(i);
	    logWarning("selectULongs", "expects to retrieve %lu elements "
		       "but only got %lu", static_cast<long unsigned>(tot),
		       static_cast<long unsigned>(i));
	}
    }
    else {
	logWarning("selectULongs", "incompatible data type");
    }
    if (ibis::gVerbose > 4) {
	timer.stop();
	long unsigned cnt = mask.cnt();
	logMessage("selectULongs", "retrieving %lu integer%s "
		   "took %g sec(CPU), %g sec(elapsed)",
		   static_cast<long unsigned>(cnt), (cnt > 1 ? "s" : ""),
		   timer.CPUTime(), timer.realTime());
    }
    return array;
} // ibis::column::selectULongs

/// Put selected values of a float column into an array.
///
/// @note Only performs safe conversion.  Conversions from 32-bit integers,
/// 64-bit integers and 64-bit floating-point values are not allowed.  A
/// nil array will be returned if the current column can not be converted.
/// @note The caller is responsible for freeing the returned array from any
/// of the selectTypes functions.
ibis::array_t<float>*
ibis::column::selectFloats(const ibis::bitvector& mask) const {
    array_t<float>* array = new array_t<float>;
    const uint32_t tot = mask.cnt();
    if (tot == 0)
	return array;
    ibis::horometer timer;
    if (ibis::gVerbose > 4)
	timer.start();

    if (m_type == FLOAT) {
#if defined(FASTBIT_PREFER_READ_ALL)
	array_t<float> prop;
	std::string sname;
	const char* fnm = dataFileName(sname);
	ibis::fileManager::ACCESS_PREFERENCE apref =
	    thePart != 0 ? thePart->accessHint(mask, sizeof(float))
	    : ibis::fileManager::MMAP_LARGE_FILES;

	int ierr = ibis::fileManager::instance().getFile(fnm, prop, apref);
	if (ierr != 0) {
	    logWarning("selectFloats",
		       "the file manager faild to retrieve the content of"
		       " the data file \"%s\"", fnm);
	    return array;
	}

	uint32_t i = 0;
	array->resize(tot);
	const uint32_t nprop = prop.size();
	ibis::bitvector::indexSet index = mask.firstIndexSet();
	if (nprop >= mask.size()) { // no need to check loop bounds
	    while (index.nIndices() > 0) {
		const ibis::bitvector::word_t *idx0 = index.indices();
		if (index.isRange()) {
		    for (uint32_t j = *idx0; j<idx0[1]; ++j, ++i) {
			(*array)[i] = (prop[j]);
		    }
		}
		else {
		    for (uint32_t j = 0; j<index.nIndices(); ++j, ++i) {
			(*array)[i] = (prop[idx0[j]]);
		    }
		}
		++ index;
	    }
	}
	else { // check loop bounds against nprop
	    while (index.nIndices() > 0) {
		const ibis::bitvector::word_t *idx0 = index.indices();
		if (*idx0 >= nprop) break;
		if (index.isRange()) {
		    for (uint32_t j = *idx0;
			 j < (idx0[1]<=nprop ? idx0[1] : nprop);
			 ++j, ++i) {
			(*array)[i] = (prop[j]);
		    }
		}
		else {
		    for (uint32_t j = 0; j<index.nIndices(); ++j, ++i) {
			if (idx0[j] < nprop)
			    (*array)[i] = (prop[idx0[j]]);
			else
			    break;
		    }
		}
		++ index;
	    }
	}

	if (i != tot) {
	    array->resize(i);
	    logWarning("selectFloats", "expects to retrieve %lu elements "
		       "but only got %lu", static_cast<long unsigned>(tot),
		       static_cast<long unsigned>(i));
	}
#else
	long ierr = selectValuesT(mask, *array);
	if (ierr < 0) {
	    LOGGER(ibis::gVerbose > 0)
		<< "Warning -- ibis::column["
		<< (thePart!=0 ? thePart->name() : "") << "." << m_name
		<< "]::selectValuesT failed with error code " << ierr;
	    delete array;
	    array = 0;
	}
#endif
    }
    else if (m_type == ibis::USHORT) {
	array_t<uint16_t> prop;
	std::string sname;
	const char* fnm = dataFileName(sname);
	ibis::fileManager::ACCESS_PREFERENCE apref =
	    thePart != 0 ? thePart->accessHint(mask, sizeof(uint16_t))
	    : ibis::fileManager::MMAP_LARGE_FILES;

	int ierr = ibis::fileManager::instance().getFile(fnm, prop, apref);
	if (ierr != 0) {
	    logWarning("selectFloats",
		       "the file manager faild to retrieve the content of"
		       " the data file \"%s\"", fnm);
	    return array;
	}

	uint32_t i = 0;
	array->resize(tot);
	const uint32_t nprop = prop.size();
	ibis::bitvector::indexSet index = mask.firstIndexSet();
	if (nprop >= mask.size()) { // no need to check loop bounds
	    while (index.nIndices() > 0) {
		const ibis::bitvector::word_t *idx0 = index.indices();
		if (index.isRange()) {
		    for (uint32_t j = *idx0; j < idx0[1]; ++j, ++i) {
			(*array)[i] = (prop[j]);
		    }
		}
		else {
		    for (uint32_t j = 0; j<index.nIndices(); ++j, ++i) {
			(*array)[i] = (prop[idx0[j]]);
		    }
		}
		++ index;
	    }
	}
	else { // need to check loop bounds against nprop
	    while (index.nIndices() > 0) {
		const ibis::bitvector::word_t *idx0 = index.indices();
		if (*idx0 >= nprop) break;
		if (index.isRange()) {
		    for (uint32_t j = *idx0;
			 j < (idx0[1]<=nprop ? idx0[1] : nprop);
			 ++j, ++i) {
			(*array)[i] = (prop[j]);
		    }
		}
		else {
		    for (uint32_t j = 0; j<index.nIndices(); ++j, ++i) {
			if (idx0[j] < nprop)
			    (*array)[i] = (prop[idx0[j]]);
			else
			    break;
		    }
		}
		++ index;
	    }
	}
	if (i != tot) {
	    array->resize(i);
	    logWarning("selectFloats", "expects to retrieve %lu elements "
		       "but only got %lu", static_cast<long unsigned>(tot),
		       static_cast<long unsigned>(i));
	}
    }
    else if (m_type == SHORT) {
	array_t<int16_t> prop;
	std::string sname;
	const char* fnm = dataFileName(sname);
	ibis::fileManager::ACCESS_PREFERENCE apref =
	    thePart != 0 ? thePart->accessHint(mask, sizeof(int16_t))
	    : ibis::fileManager::MMAP_LARGE_FILES;

	int ierr = ibis::fileManager::instance().getFile(fnm, prop, apref);
	if (ierr != 0) {
	    logWarning("selectFloats",
		       "the file manager faild to retrieve the content of"
		       " the data file \"%s\"", fnm);
	    return array;
	}

	uint32_t i = 0;
	array->resize(tot);
	const uint32_t nprop = prop.size();
	ibis::bitvector::indexSet index = mask.firstIndexSet();
	if (nprop >= mask.size()) { // no need to check loop bounds
	    while (index.nIndices() > 0) {
		const ibis::bitvector::word_t *idx0 = index.indices();
		if (index.isRange()) {
		    for (uint32_t j = *idx0; j < idx0[1]; ++j, ++i) {
			(*array)[i] = (prop[j]);
		    }
		}
		else {
		    for (uint32_t j = 0; j<index.nIndices(); ++j, ++i) {
			(*array)[i] = (prop[idx0[j]]);
		    }
		}
		++ index;
	    }
	}
	else { // need to check loop bounds against nprop
	    while (index.nIndices() > 0) {
		const ibis::bitvector::word_t *idx0 = index.indices();
		if (*idx0 >= nprop) break;
		if (index.isRange()) {
		    for (uint32_t j = *idx0;
			 j < (idx0[1]<=nprop ? idx0[1] : nprop);
			 ++j, ++i) {
			(*array)[i] = (prop[j]);
		    }
		}
		else {
		    for (uint32_t j = 0; j<index.nIndices(); ++j, ++i) {
			if (idx0[j] < nprop)
			    (*array)[i] = (prop[idx0[j]]);
			else
			    break;
		    }
		}
		++ index;
	    }
	}
	if (i != tot) {
	    array->resize(i);
	    logWarning("selectFloats", "expects to retrieve %lu elements "
		       "but only got %lu", static_cast<long unsigned>(tot),
		       static_cast<long unsigned>(i));
	}
    }
    else if (m_type == UBYTE) {
	array_t<unsigned char> prop;
	std::string sname;
	const char* fnm = dataFileName(sname);
	ibis::fileManager::ACCESS_PREFERENCE apref =
	    thePart != 0 ? thePart->accessHint(mask, sizeof(unsigned char))
	    : ibis::fileManager::MMAP_LARGE_FILES;

	int ierr = ibis::fileManager::instance().getFile(fnm, prop, apref);
	if (ierr != 0) {
	    logWarning("selectFloats",
		       "the file manager faild to retrieve the content of"
		       " the data file \"%s\"", fnm);
	    return array;
	}

	uint32_t i = 0;
	array->resize(tot);
	const uint32_t nprop = prop.size();
	ibis::bitvector::indexSet index = mask.firstIndexSet();
	if (nprop >= mask.size()) { // no need to check loop bounds
	    while (index.nIndices() > 0) {
		const ibis::bitvector::word_t *idx0 = index.indices();
		if (index.isRange()) {
		    for (uint32_t j = *idx0; j < idx0[1]; ++j, ++i) {
			(*array)[i] = (prop[j]);
		    }
		}
		else {
		    for (uint32_t j = 0; j<index.nIndices(); ++j, ++i) {
			(*array)[i] = (prop[idx0[j]]);
		    }
		}
		++ index;
	    }
	}
	else { // need to check loop bounds against nprop
	    while (index.nIndices() > 0) {
		const ibis::bitvector::word_t *idx0 = index.indices();
		if (*idx0 >= nprop) break;
		if (index.isRange()) {
		    for (uint32_t j = *idx0;
			 j < (idx0[1]<=nprop ? idx0[1] : nprop);
			 ++j, ++i) {
			(*array)[i] = (prop[j]);
		    }
		}
		else {
		    for (uint32_t j = 0; j<index.nIndices(); ++j, ++i) {
			if (idx0[j] < nprop)
			    (*array)[i] = (prop[idx0[j]]);
			else
			    break;
		    }
		}
		++ index;
	    }
	}
	if (i != tot) {
	    array->resize(i);
	    logWarning("selectFloats", "expects to retrieve %lu elements "
		       "but only got %lu", static_cast<long unsigned>(tot),
		       static_cast<long unsigned>(i));
	}
    }
    else if (m_type == BYTE) {
	array_t<char> prop;
	std::string sname;
	const char* fnm = dataFileName(sname);
	ibis::fileManager::ACCESS_PREFERENCE apref =
	    thePart != 0 ? thePart->accessHint(mask, sizeof(char))
	    : ibis::fileManager::MMAP_LARGE_FILES;

	int ierr = ibis::fileManager::instance().getFile(fnm, prop, apref);
	if (ierr != 0) {
	    logWarning("selectFloats",
		       "the file manager faild to retrieve the content of"
		       " the data file \"%s\"", fnm);
	    return array;
	}

	uint32_t i = 0;
	array->resize(tot);
	const uint32_t nprop = prop.size();
	ibis::bitvector::indexSet index = mask.firstIndexSet();
	if (nprop >= mask.size()) { // no need to check loop bounds
	    while (index.nIndices() > 0) {
		const ibis::bitvector::word_t *idx0 = index.indices();
		if (index.isRange()) {
		    for (uint32_t j = *idx0; j < idx0[1]; ++j, ++i) {
			(*array)[i] = (prop[j]);
		    }
		}
		else {
		    for (uint32_t j = 0; j<index.nIndices(); ++j, ++i) {
			(*array)[i] = (prop[idx0[j]]);
		    }
		}
		++ index;
	    }
	}
	else { // need to check loop bounds against nprop
	    while (index.nIndices() > 0) {
		const ibis::bitvector::word_t *idx0 = index.indices();
		if (*idx0 >= nprop) break;
		if (index.isRange()) {
		    for (uint32_t j = *idx0;
			 j < (idx0[1]<=nprop ? idx0[1] : nprop);
			 ++j, ++i) {
			(*array)[i] = (prop[j]);
		    }
		}
		else {
		    for (uint32_t j = 0; j<index.nIndices(); ++j, ++i) {
			if (idx0[j] < nprop)
			    (*array)[i] = (prop[idx0[j]]);
			else
			    break;
		    }
		}
		++ index;
	    }
	}
	if (i != tot) {
	    array->resize(i);
	    logWarning("selectFloats", "expects to retrieve %lu elements "
		       "but only got %lu", static_cast<long unsigned>(tot),
		       static_cast<long unsigned>(i));
	}
    }
    else {
	logWarning("selectFloats", "incompatible data type");
    }
    if (ibis::gVerbose > 4) {
	timer.stop();
	long unsigned cnt = mask.cnt();
	logMessage("selectFloats", "retrieving %lu float value%s "
		   "took %g sec(CPU), %g sec(elapsed)",
		   static_cast<long unsigned>(cnt), (cnt > 1 ? "s" : ""),
		   timer.CPUTime(), timer.realTime());
    }
    return array;
} // ibis::column::selectFloats

/// Put the selected values into an array as doubles.
///
/// @note Any numerical values can be converted to doubles, however for
/// 64-bit integers this conversion may cause lose of precision.
/// @note The caller is responsible for freeing the returned array from any
/// of the selectTypes functions.
ibis::array_t<double>*
ibis::column::selectDoubles(const ibis::bitvector& mask) const {
    array_t<double>* array = new array_t<double>;
    const uint32_t tot = mask.cnt();
    if (tot == 0)
	return array;
    ibis::horometer timer;
    if (ibis::gVerbose > 4)
	timer.start();

    switch(m_type) {
    case ibis::CATEGORY:
    case ibis::UINT: {
	array_t<uint32_t> prop;
	std::string sname;
	const char* fnm = dataFileName(sname);
	ibis::fileManager::ACCESS_PREFERENCE apref =
	    thePart != 0 ? thePart->accessHint(mask, sizeof(uint32_t))
	    : ibis::fileManager::MMAP_LARGE_FILES;

	int ierr = ibis::fileManager::instance().getFile(fnm, prop, apref);
	if (ierr != 0) {
	    logWarning("selectDoubles"
		       "the file manager faild to retrieve the content of"
		       " the data file \"%s\"", fnm);
	    return array;
	}

	uint32_t i = 0;
	array->resize(tot);
	const uint32_t nprop = prop.size();
	ibis::bitvector::indexSet index = mask.firstIndexSet();
	if (nprop >= mask.size()) {
	    while (index.nIndices() > 0) {
		const ibis::bitvector::word_t *idx0 = index.indices();
		if (index.isRange()) {
		    for (uint32_t j = *idx0; j<idx0[1]; ++j, ++i) {
			(*array)[i] = (prop[j]);
		    }
		}
		else {
		    for (uint32_t j = 0; j<index.nIndices(); ++j, ++i) {
			(*array)[i] = (prop[idx0[j]]);
		    }
		}
		++ index;
	    }
	}
	else {
	    while (index.nIndices() > 0) {
		const ibis::bitvector::word_t *idx0 = index.indices();
		if (*idx0 >= nprop) break;
		if (index.isRange()) {
		    for (uint32_t j = *idx0;
			 j<(idx0[1]<=nprop ? idx0[1] : nprop);
			 ++j, ++i) {
			(*array)[i] = (prop[j]);
		    }
		}
		else {
		    for (uint32_t j = 0; j<index.nIndices(); ++j, ++i) {
			if (idx0[j] < nprop)
			    (*array)[i] = (prop[idx0[j]]);
			else
			    break;
		    }
		}
		++ index;
	    }
	}

	if (i != tot) {
	    array->resize(i);
	    logWarning("selectDoubles", "expects to retrieve %lu elements "
		       "but only got %lu", static_cast<long unsigned>(tot),
		       static_cast<long unsigned>(i));
	}
	else if (ibis::gVerbose > 4) {
	    timer.stop();
	    long unsigned cnt = mask.cnt();
	    logMessage("selectDoubles", "retrieving %lu unsigned integer%s "
		       "took %g sec(CPU), %g sec(elapsed)",
		       static_cast<long unsigned>(cnt), (cnt > 1 ? "s" : ""),
		       timer.CPUTime(), timer.realTime());
	}
	break;}
    case ibis::INT: {
	array_t<int32_t> prop;
	std::string sname;
	const char* fnm = dataFileName(sname);
	ibis::fileManager::ACCESS_PREFERENCE apref =
	    thePart != 0 ? thePart->accessHint(mask, sizeof(int32_t))
	    : ibis::fileManager::MMAP_LARGE_FILES;

	int ierr = ibis::fileManager::instance().getFile(fnm, prop, apref);
	if (ierr != 0) {
	    logWarning("selectDoubles"
		       "the file manager faild to retrieve the content of"
		       " the data file \"%s\"", fnm);
	    return array;
	}

	uint32_t i = 0;
	array->resize(tot);
	const uint32_t nprop = prop.size();
	ibis::bitvector::indexSet index = mask.firstIndexSet();
	if (nprop >= mask.size()) {
	    while (index.nIndices() > 0) {
		const ibis::bitvector::word_t *idx0 = index.indices();
		if (index.isRange()) {
		    for (uint32_t j = *idx0; j<idx0[1]; ++j, ++i) {
			(*array)[i] = (prop[j]);
		    }
		}
		else {
		    for (uint32_t j = 0; j<index.nIndices(); ++j, ++i) {
			(*array)[i] = (prop[idx0[j]]);
		    }
		}
		++ index;
	    }
	}
	else {
	    while (index.nIndices() > 0) {
		const ibis::bitvector::word_t *idx0 = index.indices();
		if (*idx0 >= nprop) break;
		if (index.isRange()) {
		    for (uint32_t j = *idx0;
			 j<(idx0[1]<=nprop ? idx0[1] : nprop);
			 ++j, ++i) {
			(*array)[i] = (prop[j]);
		    }
		}
		else {
		    for (uint32_t j = 0; j<index.nIndices(); ++j, ++i) {
			if (idx0[j] < nprop)
			    (*array)[i] = (prop[idx0[j]]);
			else
			    break;
		    }
		}
		++ index;
	    }
	}

	if (i != tot) {
	    array->resize(i);
	    logWarning("selectDoubles", "expects to retrieve %lu elements "
		       "but only got %lu", static_cast<long unsigned>(tot),
		       static_cast<long unsigned>(i));
	}
	else if (ibis::gVerbose > 4) {
	    timer.stop();
	    long unsigned cnt = mask.cnt();
	    logMessage("selectDoubles", "retrieving %lu integer%s "
		       "took %g sec(CPU), %g sec(elapsed)",
		       static_cast<long unsigned>(cnt), (cnt > 1 ? "s" : ""),
		       timer.CPUTime(), timer.realTime());
	}
	break;}
    case ibis::USHORT: {
	array_t<uint16_t> prop;
	std::string sname;
	const char* fnm = dataFileName(sname);
	ibis::fileManager::ACCESS_PREFERENCE apref =
	    thePart != 0 ? thePart->accessHint(mask, sizeof(int16_t))
	    : ibis::fileManager::MMAP_LARGE_FILES;

	int ierr = ibis::fileManager::instance().getFile(fnm, prop, apref);
	if (ierr != 0) {
	    logWarning("selectDoubles"
		       "the file manager faild to retrieve the content of"
		       " the data file \"%s\"", fnm);
	    return array;
	}

	uint32_t i = 0;
	array->resize(tot);
	const uint32_t nprop = prop.size();
	ibis::bitvector::indexSet index = mask.firstIndexSet();
	if (nprop >= mask.size()) {
	    while (index.nIndices() > 0) {
		const ibis::bitvector::word_t *idx0 = index.indices();
		if (index.isRange()) {
		    for (uint32_t j = *idx0; j<idx0[1]; ++j, ++i) {
			(*array)[i] = (prop[j]);
		    }
		}
		else {
		    for (uint32_t j = 0; j<index.nIndices(); ++j, ++i) {
			(*array)[i] = (prop[idx0[j]]);
		    }
		}
		++ index;
	    }
	}
	else {
	    while (index.nIndices() > 0) {
		const ibis::bitvector::word_t *idx0 = index.indices();
		if (*idx0 >= nprop) break;
		if (index.isRange()) {
		    for (uint32_t j = *idx0;
			 j<(idx0[1]<=nprop ? idx0[1] : nprop);
			 ++j, ++i) {
			(*array)[i] = (prop[j]);
		    }
		}
		else {
		    for (uint32_t j = 0; j<index.nIndices(); ++j, ++i) {
			if (idx0[j] < nprop)
			    (*array)[i] = (prop[idx0[j]]);
			else
			    break;
		    }
		}
		++ index;
	    }
	}

	if (i != tot) {
	    array->resize(i);
	    logWarning("selectDoubles", "expects to retrieve %lu elements "
		       "but only got %lu", static_cast<long unsigned>(tot),
		       static_cast<long unsigned>(i));
	}
	else if (ibis::gVerbose > 4) {
	    timer.stop();
	    long unsigned cnt = mask.cnt();
	    logMessage("selectDoubles", "retrieving %lu unsigned short "
		       "integer%s took %g sec(CPU), %g sec(elapsed)",
		       static_cast<long unsigned>(cnt), (cnt > 1 ? "s" : ""),
		       timer.CPUTime(), timer.realTime());
	}
	break;}
    case ibis::SHORT: {
	array_t<int16_t> prop;
	std::string sname;
	const char* fnm = dataFileName(sname);
	ibis::fileManager::ACCESS_PREFERENCE apref =
	    thePart != 0 ? thePart->accessHint(mask, sizeof(int16_t))
	    : ibis::fileManager::MMAP_LARGE_FILES;

	int ierr = ibis::fileManager::instance().getFile(fnm, prop, apref);
	if (ierr != 0) {
	    logWarning("selectDoubles"
		       "the file manager faild to retrieve the content of"
		       " the data file \"%s\"", fnm);
	    return array;
	}

	uint32_t i = 0;
	array->resize(tot);
	const uint32_t nprop = prop.size();
	ibis::bitvector::indexSet index = mask.firstIndexSet();
	if (nprop >= mask.size()) {
	    while (index.nIndices() > 0) {
		const ibis::bitvector::word_t *idx0 = index.indices();
		if (index.isRange()) {
		    for (uint32_t j = *idx0; j<idx0[1]; ++j, ++i) {
			(*array)[i] = (prop[j]);
		    }
		}
		else {
		    for (uint32_t j = 0; j<index.nIndices(); ++j, ++i) {
			(*array)[i] = (prop[idx0[j]]);
		    }
		}
		++ index;
	    }
	}
	else {
	    while (index.nIndices() > 0) {
		const ibis::bitvector::word_t *idx0 = index.indices();
		if (*idx0 >= nprop) break;
		if (index.isRange()) {
		    for (uint32_t j = *idx0;
			 j<(idx0[1]<=nprop ? idx0[1] : nprop);
			 ++j, ++i) {
			(*array)[i] = (prop[j]);
		    }
		}
		else {
		    for (uint32_t j = 0; j<index.nIndices(); ++j, ++i) {
			if (idx0[j] < nprop)
			    (*array)[i] = (prop[idx0[j]]);
			else
			    break;
		    }
		}
		++ index;
	    }
	}

	if (i != tot) {
	    array->resize(i);
	    logWarning("selectDoubles", "expects to retrieve %lu elements "
		       "but only got %lu", static_cast<long unsigned>(tot),
		       static_cast<long unsigned>(i));
	}
	else if (ibis::gVerbose > 4) {
	    timer.stop();
	    long unsigned cnt = mask.cnt();
	    logMessage("selectDoubles", "retrieving %lu short integer%s "
		       "took %g sec(CPU), %g sec(elapsed)",
		       static_cast<long unsigned>(cnt), (cnt > 1 ? "s" : ""),
		       timer.CPUTime(), timer.realTime());
	}
	break;}
    case ibis::UBYTE: {
	array_t<unsigned char> prop;
	std::string sname;
	const char* fnm = dataFileName(sname);
	ibis::fileManager::ACCESS_PREFERENCE apref =
	    thePart != 0 ? thePart->accessHint(mask, sizeof(unsigned char))
	    : ibis::fileManager::MMAP_LARGE_FILES;

	int ierr = ibis::fileManager::instance().getFile(fnm, prop, apref);
	if (ierr != 0) {
	    logWarning("selectDoubles"
		       "the file manager faild to retrieve the content of"
		       " the data file \"%s\"", fnm);
	    return array;
	}

	uint32_t i = 0;
	array->resize(tot);
	const uint32_t nprop = prop.size();
	ibis::bitvector::indexSet index = mask.firstIndexSet();
	if (nprop >= mask.size()) {
	    while (index.nIndices() > 0) {
		const ibis::bitvector::word_t *idx0 = index.indices();
		if (index.isRange()) {
		    for (uint32_t j = *idx0; j<idx0[1]; ++j, ++i) {
			(*array)[i] = (prop[j]);
		    }
		}
		else {
		    for (uint32_t j = 0; j<index.nIndices(); ++j, ++i) {
			(*array)[i] = (prop[idx0[j]]);
		    }
		}
		++ index;
	    }
	}
	else {
	    while (index.nIndices() > 0) {
		const ibis::bitvector::word_t *idx0 = index.indices();
		if (*idx0 >= nprop) break;
		if (index.isRange()) {
		    for (uint32_t j = *idx0;
			 j<(idx0[1]<=nprop ? idx0[1] : nprop);
			 ++j, ++i) {
			(*array)[i] = (prop[j]);
		    }
		}
		else {
		    for (uint32_t j = 0; j<index.nIndices(); ++j, ++i) {
			if (idx0[j] < nprop)
			    (*array)[i] = (prop[idx0[j]]);
			else
			    break;
		    }
		}
		++ index;
	    }
	}

	if (i != tot) {
	    array->resize(i);
	    logWarning("selectDoubles", "expects to retrieve %lu elements "
		       "but only got %lu", static_cast<long unsigned>(tot),
		       static_cast<long unsigned>(i));
	}
	else if (ibis::gVerbose > 4) {
	    timer.stop();
	    long unsigned cnt = mask.cnt();
	    logMessage("selectDoubles", "retrieving %lu unsigned 1-byte "
		       "integer%s took %g sec(CPU), %g sec(elapsed)",
		       static_cast<long unsigned>(cnt), (cnt > 1 ? "s" : ""),
		       timer.CPUTime(), timer.realTime());
	}
	break;}
    case ibis::BYTE: {
	array_t<char> prop;
	std::string sname;
	const char* fnm = dataFileName(sname);
	ibis::fileManager::ACCESS_PREFERENCE apref =
	    thePart != 0 ? thePart->accessHint(mask, sizeof(char))
	    : ibis::fileManager::MMAP_LARGE_FILES;

	int ierr = ibis::fileManager::instance().getFile(fnm, prop, apref);
	if (ierr != 0) {
	    logWarning("selectDoubles"
		       "the file manager faild to retrieve the content of"
		       " the data file \"%s\"", fnm);
	    return array;
	}

	uint32_t i = 0;
	array->resize(tot);
	const uint32_t nprop = prop.size();
	ibis::bitvector::indexSet index = mask.firstIndexSet();
	if (nprop >= mask.size()) {
	    while (index.nIndices() > 0) {
		const ibis::bitvector::word_t *idx0 = index.indices();
		if (index.isRange()) {
		    for (uint32_t j = *idx0; j<idx0[1]; ++j, ++i) {
			(*array)[i] = (prop[j]);
		    }
		}
		else {
		    for (uint32_t j = 0; j<index.nIndices(); ++j, ++i) {
			(*array)[i] = (prop[idx0[j]]);
		    }
		}
		++ index;
	    }
	}
	else {
	    while (index.nIndices() > 0) {
		const ibis::bitvector::word_t *idx0 = index.indices();
		if (*idx0 >= nprop) break;
		if (index.isRange()) {
		    for (uint32_t j = *idx0;
			 j<(idx0[1]<=nprop ? idx0[1] : nprop);
			 ++j, ++i) {
			(*array)[i] = (prop[j]);
		    }
		}
		else {
		    for (uint32_t j = 0; j<index.nIndices(); ++j, ++i) {
			if (idx0[j] < nprop)
			    (*array)[i] = (prop[idx0[j]]);
			else
			    break;
		    }
		}
		++ index;
	    }
	}

	if (i != tot) {
	    array->resize(i);
	    logWarning("selectDoubles", "expects to retrieve %lu elements "
		       "but only got %lu", static_cast<long unsigned>(tot),
		       static_cast<long unsigned>(i));
	}
	else if (ibis::gVerbose > 4) {
	    timer.stop();
	    long unsigned cnt = mask.cnt();
	    logMessage("selectDoubles", "retrieving %lu 1-byte integer%s "
		       "took %g sec(CPU), %g sec(elapsed)",
		       static_cast<long unsigned>(cnt), (cnt > 1 ? "s" : ""),
		       timer.CPUTime(), timer.realTime());
	}
	break;}
    case ibis::FLOAT: {
	array_t<float> prop;
	std::string sname;
	const char* fnm = dataFileName(sname);
	ibis::fileManager::ACCESS_PREFERENCE apref =
	    thePart != 0 ? thePart->accessHint(mask, sizeof(float))
	    : ibis::fileManager::MMAP_LARGE_FILES;

	int ierr = ibis::fileManager::instance().getFile(fnm, prop, apref);
	if (ierr != 0) {
	    logWarning("selectDoubles"
		       "the file manager faild to retrieve the content of"
		       " the data file \"%s\"", fnm);
	    return array;
	}

	uint32_t i = 0;
	array->resize(tot);
	const uint32_t nprop = prop.size();
	ibis::bitvector::indexSet index = mask.firstIndexSet();
	if (nprop >= mask.size()) {
	    while (index.nIndices() > 0) {
		const ibis::bitvector::word_t *idx0 = index.indices();
		if (index.isRange()) {
		    for (uint32_t j = *idx0; j<idx0[1]; ++j, ++i) {
			(*array)[i] = (prop[j]);
		    }
		}
		else {
		    for (uint32_t j = 0; j<index.nIndices(); ++j, ++i) {
			(*array)[i] = (prop[idx0[j]]);
		    }
		}
		++ index;
	    }
	}
	else {
	    while (index.nIndices() > 0) {
		const ibis::bitvector::word_t *idx0 = index.indices();
		if (*idx0 >= nprop) break;
		if (index.isRange()) {
		    for (uint32_t j = *idx0;
			 j<(idx0[1]<=nprop ? idx0[1] : nprop);
			 ++j, ++i) {
			(*array)[i] = (prop[j]);
		    }
		}
		else {
		    for (uint32_t j = 0; j<index.nIndices(); ++j, ++i) {
			if (idx0[j] < nprop)
			    (*array)[i] = (prop[idx0[j]]);
			else
			    break;
		    }
		}
		++ index;
	    }
	}

	if (i != tot) {
	    array->resize(i);
	    logWarning("selectDoubles", "expects to retrieve %lu elements "
		       "but only got %lu", static_cast<long unsigned>(tot),
		       static_cast<long unsigned>(i));
	}
	else if (ibis::gVerbose > 4) {
	    timer.stop();
	    long unsigned cnt = mask.cnt();
	    logMessage("selectDoubles", "retrieving %lu float value%s "
		       "took %g sec(CPU), %g sec(elapsed)",
		       static_cast<long unsigned>(cnt), (cnt > 1 ? "s" : ""),
		       timer.CPUTime(), timer.realTime());
	}
	break;}
    case ibis::DOUBLE: {
#if defined(FASTBIT_PREFER_READ_ALL)
	array_t<double> prop;
	std::string sname;
	const char* fnm = dataFileName(sname);
	ibis::fileManager::ACCESS_PREFERENCE apref =
	    thePart != 0 ? thePart->accessHint(mask, sizeof(double))
	    : ibis::fileManager::MMAP_LARGE_FILES;

	int ierr = ibis::fileManager::instance().getFile(fnm, prop, apref);
	if (ierr != 0) {
	    logWarning("selectDoubles"
		       "the file manager faild to retrieve the content of"
		       " the data file \"%s\"", fnm);
	    return array;
	}

	uint32_t i = 0;
	array->resize(tot);
	const uint32_t nprop = prop.size();
	ibis::bitvector::indexSet index = mask.firstIndexSet();
	if (nprop >= mask.size()) {
	    while (index.nIndices() > 0) {
		const ibis::bitvector::word_t *idx0 = index.indices();
		if (index.isRange()) {
		    for (uint32_t j = *idx0; j<idx0[1]; ++j, ++i) {
			(*array)[i] = (prop[j]);
		    }
		}
		else {
		    for (uint32_t j = 0; j<index.nIndices(); ++j, ++i) {
			(*array)[i] = (prop[idx0[j]]);
		    }
		}
		++ index;
	    }
	}
	else {
	    while (index.nIndices() > 0) {
		const ibis::bitvector::word_t *idx0 = index.indices();
		if (*idx0 >= nprop) break;
		if (index.isRange()) {
		    for (uint32_t j = *idx0;
			 j<(idx0[1]<=nprop ? idx0[1] : nprop);
			 ++j, ++i) {
			(*array)[i] = (prop[j]);
		    }
		}
		else {
		    for (uint32_t j = 0; j<index.nIndices(); ++j, ++i) {
			if (idx0[j] < nprop)
			    (*array)[i] = (prop[idx0[j]]);
			else
			    break;
		    }
		}
		++ index;
	    }
	}

	if (i != tot) {
	    array->resize(i);
	    logWarning("selectDoubles", "expects to retrieve %lu elements "
		       "but only got %lu", static_cast<long unsigned>(tot),
		       static_cast<long unsigned>(i));
	}
	else if (ibis::gVerbose > 4) {
	    timer.stop();
	    long unsigned cnt = mask.cnt();
	    logMessage("selectDoubles", "retrieving %lu double value%s "
		       "took %g sec(CPU), %g sec(elapsed)",
		       static_cast<long unsigned>(cnt), (cnt > 1 ? "s" : ""),
		       timer.CPUTime(), timer.realTime());
	}
#else
	long ierr = selectValuesT(mask, *array);
	if (ierr < 0) {
	    LOGGER(ibis::gVerbose > 0)
		<< "Warning -- ibis::column["
		<< (thePart!=0 ? thePart->name() : "") << "." << m_name
		<< "]::selectValuesT failed with error code " << ierr;
	    delete array;
	    array = 0;
	}
#endif
	break;}
    default: {
	logWarning("selectDoubles", "incompatible data type");
	break;}
    }
    return array;
} // ibis::column::selectDoubles

/// Select the values marked in the bitvector @c mask.  Select all values
/// marked 1 in the @c mask and pack them into the output array @c vals and
/// fill the array @c inds with the positions of the values selected.
///
/// On a successful executation, it returns the number of values selected.
/// If it returns zero (0), the contents of @c vals is not modified.  If it
/// returns a negative number, the contents of arrays @c vals is not
/// guaranteed to be in any particular state.
template <typename T>
long ibis::column::selectValuesT(const bitvector& mask,
				 ibis::array_t<T>& vals) const {
    if (thePart == 0) return -2;
    long ierr = 0;
    const long unsigned tot = mask.cnt();
    if (mask.cnt() == 0) return ierr;
    if (elementSize() != sizeof(T)) {
	logWarning("selectValuesT", "incompatible types");
	return -1;
    }
    std::string sname;
    const char *dfn = dataFileName(sname);
    if (dfn == 0) return -3;
#ifdef DEBUG
    logMessage("selectValuesT", "selecting %lu out of %lu values from %s",
	       tot, static_cast<long unsigned>
	       (thePart != 0 ? thePart->nRows() : 0), dfn);
#endif
    if (tot == mask.size()) { // read all values
	ierr = ibis::fileManager::instance().getFile(dfn, vals);
	return ierr;
    }

    try {
	vals.reserve(tot);
    }
    catch (...) {
	LOGGER(ibis::gVerbose > 1)
	    << "Warning -- ibis::column["
	    << (thePart!=0 ? thePart->name() : "") << "." << m_name
	    << "]::selectValuesT failed to allocate space for vals[" << tot
	    << "]";
	return -2;
    }
    ibis::fileManager::storage *raw = 0;
    // mask.size()*sizeof(T)/pagesize() -- number of pages
    // mask.bytes()/sizeof(uint32_t) -- number of words (i.e. possible
    // seeks to perform a read)
    if (mask.size() >= 1048576 && tot+tot <= mask.size() && mask.bytes()/4 <
	mask.size()*sizeof(T)/ibis::fileManager::pageSize()/8) {
	// if the file is in memory, use it
	ibis::fileManager::ACCESS_PREFERENCE apref =
	    thePart != 0 ? thePart->accessHint(mask, sizeof(T))
	    : ibis::fileManager::MMAP_LARGE_FILES;
	ierr = ibis::fileManager::instance().tryGetFile(dfn, &raw, apref);
    }
    else {
	// attempt to read the whole file into memory
	ibis::fileManager::ACCESS_PREFERENCE apref =
	    thePart != 0 ? thePart->accessHint(mask, sizeof(T))
	    : ibis::fileManager::MMAP_LARGE_FILES;
	ierr = ibis::fileManager::instance().getFile(dfn, &raw, apref);
    }
    if (ierr == 0) { // the file is in memory
	// the content of raw is automatically deallocated through the
	// destructor of incore
	array_t<T> incore(raw); // make the raw storage more friendly
	const uint32_t nr = (incore.size() <= mask.size() ?
			     incore.size() : mask.size());
	for (ibis::bitvector::indexSet ix = mask.firstIndexSet();
	     ix.nIndices() > 0; ++ ix) {
	    const ibis::bitvector::word_t *ixval = ix.indices();
	    if (ix.isRange()) {
		const uint32_t stop = (ixval[1] <= nr ? ixval[1] : nr);
		for (uint32_t i = *ixval; i < stop; ++ i) {
		    vals.push_back(incore[i]);
		}
	    }
	    else {
		for (uint32_t j = 0; j < ix.nIndices(); ++ j) {
		    if (ixval[j] < nr) {
			vals.push_back(incore[ixval[j]]);
		    }
		    else {
			break;
		    }
		}
	    }
	}
	if (ibis::gVerbose > 4)
	    logMessage("selectValuesT", "got %lu values (%lu wanted) from an "
		       "in-memory version of file %s as %s",
		       static_cast<long unsigned>(vals.size()), tot, dfn,
		       typeid(T).name());
    }
    else { // has to use UnixRead family of functions
	int fdes = UnixOpen(dfn, OPEN_READONLY);
	if (fdes < 0) {
	    logWarning("selectValuesT", "failed to open file %s, ierr=%d",
		       dfn, fdes);
	    return fdes;
	}
	int32_t pos = UnixSeek(fdes, 0L, SEEK_END) / sizeof(T);
	const uint32_t nr = (pos <= static_cast<int32_t>(thePart->nRows()) ?
			     pos : thePart->nRows());

	for (ibis::bitvector::indexSet ix = mask.firstIndexSet();
	     ix.nIndices() > 0; ++ ix) {
	    const ibis::bitvector::word_t *ixval = ix.indices();
	    if (ix.isRange()) {
		// read the whole group in one-shot
		pos = UnixSeek(fdes, *ixval * sizeof(T), SEEK_SET);
		const uint32_t nelm = (ixval[1]-ixval[0] <= nr-vals.size() ?
				       ixval[1]-ixval[0] : nr-vals.size());
		ierr = UnixRead(fdes, vals.begin()+vals.size(),
				nelm*sizeof(T));
		if (ierr > 0) {
		    ierr /=  sizeof(T);
		    vals.resize(vals.size() + ierr);
		    ibis::fileManager::instance().recordPages(pos, pos+ierr);
		    if (static_cast<uint32_t>(ierr) != nelm)
			logWarning("selectValuesT", "expected to read %lu "
				   "consecutive elements (of %lu bytes each) "
				   "from %s, but actually read %ld",
				   static_cast<long unsigned>(nelm),
				   static_cast<long unsigned>(sizeof(T)),
				   dfn, ierr);
		}
		else {
		    logWarning("selectValuesT", "failed to read at %ld in "
			       "file %s", UnixSeek(fdes, 0L, SEEK_CUR),
			       dfn);
		}
	    }
	    else {
		// read each value separately
		for (uint32_t j = 0; j < ix.nIndices(); ++j) {
		    const int32_t target = ixval[j] * sizeof(T);
		    pos = UnixSeek(fdes, target, SEEK_SET);
		    if (pos == target) {
			T tmp;
			ierr = UnixRead(fdes, &tmp, sizeof(tmp));
			if (ierr == sizeof(tmp)) {
			    vals.push_back(tmp);
			}
			else {
			    logWarning("selectValuesT", "failed to read "
				       "%lu-byte data from offset %ld in "
				       "file \"%s\"",
				       static_cast<long unsigned>(sizeof(tmp)),
				       static_cast<long>(target), dfn);
			}
		    }
		    else {
			logWarning("selectValuesT", "failed to seek to the "
				   "expected location in file \"%s\" (actual "
				   "%ld, expected %ld)", dfn,
				   static_cast<long>(pos),
				   static_cast<long>(target));
		    }
		}
	    }
	} // for (ibis::bitvector::indexSet...
	if (ibis::gVerbose > 4)
	    logMessage("selectValuesT", "got %lu values (%lu wanted) from "
		       "reading file %s",
		       static_cast<long unsigned>(vals.size()),
		       static_cast<long unsigned>(tot), dfn);
    }

    ierr = vals.size();
    if (vals.size() != tot)
	logWarning("selectValuesT", "got %li out of %lu values from %s as %s",
		   ierr, static_cast<long unsigned>(tot), dfn,
		   typeid(T).name());
    return ierr;
} // ibis::column::selectValuesT

/// Select the values marked in the bitvector @c mask.
/// Select all values marked 1 in the @c mask and pack them into the
/// output array @c vals and fill the array @c inds with the positions
/// of the values selected.  On a successful executation, it returns
/// the number of values selected.  If it returns zero (0), the
/// contents of @c vals and @c inds are not modified.  If it returns a
/// negative number, the contents of arrays @c vals and @c inds are not
/// guaranteed to be in particular state.
template <typename T>
long ibis::column::selectValuesT(const bitvector& mask,
				 ibis::array_t<T>& vals,
				 ibis::array_t<uint32_t>& inds) const {
    if (thePart == 0) return -2;
    long ierr = 0;
    const long unsigned tot = mask.cnt();
    if (mask.cnt() == 0) return ierr;
    if (elementSize() != sizeof(T)) {
	logWarning("selectValuesT", "incompatible types");
	return -1;
    }

    try {
	vals.reserve(tot);
	inds.reserve(tot);
    }
    catch (...) {
	LOGGER(ibis::gVerbose > 1)
	    << "Warning -- ibis::column["
	    << (thePart!=0 ? thePart->name() : "") << "." << m_name
	    << "]::selectValuesT failed to allocate space for vals[" << tot
	    << "] and inds[" << tot << "]";
	return -2;
    }
    std::string sname;
    const char *dfn = dataFileName(sname);
    if (dfn == 0) return -3;
#ifdef DEBUG
    logMessage("selectValuesT", "selecting %lu out of %lu values from %s",
	       tot, static_cast<long unsigned>
	       (thePart != 0 ? thePart->nRows() : 0), dfn);
#endif
    ibis::fileManager::storage *raw = 0;
    // mask.size()*sizeof(T)/pagesize() -- number of pages
    // mask.bytes()/sizeof(uint32_t) -- number of words (i.e. possible
    // seeks to perform a read)
    if (mask.size() >= 1048576 && tot+tot <= mask.size() && mask.bytes()/4 <
	mask.size()*sizeof(T)/ibis::fileManager::pageSize()/8) {
	// if the file is in memory, use it
	ibis::fileManager::ACCESS_PREFERENCE apref =
	    thePart != 0 ? thePart->accessHint(mask, sizeof(T))
	    : ibis::fileManager::MMAP_LARGE_FILES;
	ierr = ibis::fileManager::instance().tryGetFile(dfn, &raw, apref);
    }
    else {
	// attempt to read the whole file into memory
	ibis::fileManager::ACCESS_PREFERENCE apref =
	    thePart != 0 ? thePart->accessHint(mask, sizeof(T))
	    : ibis::fileManager::MMAP_LARGE_FILES;
	ierr = ibis::fileManager::instance().getFile(dfn, &raw, apref);
    }
    if (ierr == 0) { // the file is in memory
	// the content of raw is automatically deallocated through the
	// destructor of incore
	array_t<T> incore(raw); // make the raw storage more friendly
	const uint32_t nr = (incore.size() <= mask.size() ?
			     incore.size() : mask.size());
	for (ibis::bitvector::indexSet ix = mask.firstIndexSet();
	     ix.nIndices() > 0; ++ ix) {
	    const ibis::bitvector::word_t *ixval = ix.indices();
	    if (ix.isRange()) {
		const uint32_t stop = (ixval[1] <= nr ? ixval[1] : nr);
		for (uint32_t i = *ixval; i < stop; ++ i) {
		    vals.push_back(incore[i]);
		    inds.push_back(i);
		}
	    }
	    else {
		for (uint32_t j = 0; j < ix.nIndices(); ++ j) {
		    if (ixval[j] < nr) {
			vals.push_back(incore[ixval[j]]);
			inds.push_back(ixval[j]);
		    }
		    else {
			break;
		    }
		}
	    }
	}
	if (ibis::gVerbose > 4)
	    logMessage("selectValuesT", "got %lu values (%lu wanted) from an "
		       "in-memory version of file %s as %s",
		       static_cast<long unsigned>(vals.size()), tot, dfn,
		       typeid(T).name());
    }
    else { // has to use UnixRead family of functions
	int fdes = UnixOpen(dfn, OPEN_READONLY);
	if (fdes < 0) {
	    logWarning("selectValuesT", "failed to open file %s, ierr=%d",
		       dfn, fdes);
	    return fdes;
	}
	int32_t pos = UnixSeek(fdes, 0L, SEEK_END) / sizeof(T);
	const uint32_t nr = (pos <= static_cast<int32_t>(thePart->nRows()) ?
			     pos : thePart->nRows());

	for (ibis::bitvector::indexSet ix = mask.firstIndexSet();
	     ix.nIndices() > 0; ++ ix) {
	    const ibis::bitvector::word_t *ixval = ix.indices();
	    if (ix.isRange()) {
		// read the whole group in one-shot
		pos = UnixSeek(fdes, *ixval * sizeof(T), SEEK_SET);
		const uint32_t nelm = (ixval[1]-ixval[0] <= nr-vals.size() ?
				       ixval[1]-ixval[0] : nr-vals.size());
		ierr = UnixRead(fdes, vals.begin()+vals.size(),
				nelm*sizeof(T));
		if (ierr > 0) {
		    ierr /=  sizeof(T);
		    vals.resize(vals.size() + ierr);
		    for (int i = 0; i < ierr; ++ i)
			inds.push_back(i + *ixval);
		    ibis::fileManager::instance().recordPages(pos, pos+ierr);
		    if (static_cast<uint32_t>(ierr) != nelm)
			logWarning("selectValuesT", "expected to read %lu "
				   "consecutive elements (of %lu bytes each) "
				   "from %s, but actually read %ld",
				   static_cast<long unsigned>(nelm),
				   static_cast<long unsigned>(sizeof(T)),
				   dfn, ierr);
		}
		else {
		    logWarning("selectValuesT", "failed to read at %ld in "
			       "file %s", UnixSeek(fdes, 0L, SEEK_CUR),
			       dfn);
		}
	    }
	    else {
		// read each value separately
		for (uint32_t j = 0; j < ix.nIndices(); ++j) {
		    const int32_t target = ixval[j] * sizeof(T);
		    pos = UnixSeek(fdes, target, SEEK_SET);
		    if (pos == target) {
			T tmp;
			ierr = UnixRead(fdes, &tmp, sizeof(tmp));
			if (ierr == sizeof(tmp)) {
			    vals.push_back(tmp);
			    inds.push_back(ixval[j]);
			}
			else {
			    logWarning("selectValuesT", "failed to read "
				       "%lu-byte data from offset %ld in "
				       "file \"%s\"",
				       static_cast<long unsigned>(sizeof(tmp)),
				       static_cast<long>(target), dfn);
			}
		    }
		    else {
			logWarning("selectValuesT", "failed to seek to the "
				   "expected location in file \"%s\" (actual "
				   "%ld, expected %ld)", dfn,
				   static_cast<long>(pos),
				   static_cast<long>(target));
		    }
		}
	    }
	} // for (ibis::bitvector::indexSet...
	if (ibis::gVerbose > 4)
	    logMessage("selectValuesT", "got %lu values (%lu wanted) from "
		       "reading file %s",
		       static_cast<long unsigned>(vals.size()),
		       static_cast<long unsigned>(tot), dfn);
    }

    ierr = (vals.size() <= inds.size() ? vals.size() : inds.size());
    if (static_cast<uint32_t>(ierr) != tot)
	logWarning("selectValuesT", "got %li out of %lu values from %s as %s",
		   ierr, static_cast<long unsigned>(tot), dfn,
		   typeid(T).name());
    vals.resize(ierr);
    inds.resize(ierr);
    return ierr;
} // ibis::column::selectValuesT

/// The caller must provide the correct array_t<type>* for vals!  Not type
/// casting is performed in this function.  Only elementary numerical types
/// are supported.
long ibis::column::selectValues(const bitvector& mask, void* vals) const {
    if (vals == 0) return -1L;
    switch (m_type) {
    case ibis::BYTE:
	return selectValuesT(mask, *static_cast<array_t<char>*>(vals));
    case ibis::UBYTE:
	return selectValuesT(mask, *static_cast<array_t<unsigned char>*>(vals));
    case ibis::SHORT:
	return selectValuesT(mask, *static_cast<array_t<int16_t>*>(vals));
    case ibis::USHORT:
	return selectValuesT(mask, *static_cast<array_t<uint16_t>*>(vals));
    case ibis::INT:
	return selectValuesT(mask, *static_cast<array_t<int32_t>*>(vals));
    case ibis::UINT:
	return selectValuesT(mask, *static_cast<array_t<uint32_t>*>(vals));
    case ibis::LONG:
	return selectValuesT(mask, *static_cast<array_t<int64_t>*>(vals));
    case ibis::ULONG:
	return selectValuesT(mask, *static_cast<array_t<uint64_t>*>(vals));
    case ibis::FLOAT:
	return selectValuesT(mask, *static_cast<array_t<float>*>(vals));
    case ibis::DOUBLE:
	return selectValuesT(mask, *static_cast<array_t<double>*>(vals));
    case ibis::OID:
	return selectValuesT(mask, *static_cast<array_t<ibis::rid_t>*>(vals));
    default:
	LOGGER(ibis::gVerbose >= 0)
	    << "Warning -- column::selectValues["
	    << (thePart!=0 ? thePart->name() : "") << "." << m_name
	    << "]::selectValues is not able to handle data type "
	    << ibis::TYPESTRING[(int)m_type];
	return -2L;
    }
} // ibis::column::selectValues

/// The caller must provide the correct array_t<type>* for vals!  Not type
/// casting is performed in this function.  Only elementary numerical types
/// are supported.
long ibis::column::selectValues(const bitvector& mask, void* vals,
				ibis::array_t<uint32_t>& inds) const {
    if (vals == 0) return -1L;
    switch (m_type) {
    case ibis::BYTE:
	return selectValuesT(mask, *static_cast<array_t<char>*>(vals), inds);
    case ibis::UBYTE:
	return selectValuesT(mask, *static_cast<array_t<unsigned char>*>(vals),
			     inds);
    case ibis::SHORT:
	return selectValuesT(mask, *static_cast<array_t<int16_t>*>(vals), inds);
    case ibis::USHORT:
	return selectValuesT(mask, *static_cast<array_t<uint16_t>*>(vals), inds);
    case ibis::INT:
	return selectValuesT(mask, *static_cast<array_t<int32_t>*>(vals), inds);
    case ibis::UINT:
	return selectValuesT(mask, *static_cast<array_t<uint32_t>*>(vals), inds);
    case ibis::LONG:
	return selectValuesT(mask, *static_cast<array_t<int64_t>*>(vals), inds);
    case ibis::ULONG:
	return selectValuesT(mask, *static_cast<array_t<uint64_t>*>(vals), inds);
    case ibis::FLOAT:
	return selectValuesT(mask, *static_cast<array_t<float>*>(vals), inds);
    case ibis::DOUBLE:
	return selectValuesT(mask, *static_cast<array_t<double>*>(vals), inds);
    case ibis::OID:
	return selectValuesT(mask, *static_cast<array_t<ibis::rid_t>*>(vals),
	    inds);
    default:
	LOGGER(ibis::gVerbose >= 0)
	    << "Warning -- column::selectValues["
	    << (thePart!=0 ? thePart->name() : "") << "." << m_name
	    << "]::selectValues is not able to handle data type "
	    << ibis::TYPESTRING[(int)m_type];
	return -2L;
    }
} // ibis::column::selectValues

// only write some information about the column
void ibis::column::print(std::ostream& out) const {
    out << m_name.c_str() << ": " << m_desc.c_str()
	<< " (" << ibis::TYPESTRING[(int)m_type] << ") [" << lower << ", "
	<< upper << "]";
} // ibis::column::print

// three error logging functions
void ibis::column::logError(const char* event, const char* fmt, ...) const {
#if (defined(HAVE_VPRINTF) || defined(_WIN32)) && ! defined(DISABLE_VPRINTF)
    char* s = new char[strlen(fmt)+MAX_LINE];
    if (s != 0) {
	va_list args;
	va_start(args, fmt);
	vsprintf(s, fmt, args);
	va_end(args);

	{ // make sure the message is written before throwing
	    ibis::util::logger lg;
	    lg.buffer() << " Error *** column["
			<< (thePart != 0 ? thePart->name() : "")
			<< '.' << m_name.c_str() << "]("
			<< ibis::TYPESTRING[(int)m_type] << ")::" << event
			<< " -- " << s;
	    if (errno != 0)
		lg.buffer() << " ... " << strerror(errno);
	}
	throw s;
    }
    else {
#endif
	{
	    ibis::util::logger lg;
	    lg.buffer() <<  " Error *** column["
			<< (thePart != 0 ? thePart->name() : "")
			<< '.' << m_name.c_str() << "]("
			<< ibis::TYPESTRING[(int)m_type] << ")::" << event
			<< " -- " << fmt;
	    if (errno != 0)
		lg.buffer() << " ... " << strerror(errno);
	}
	throw fmt;
#if (defined(HAVE_VPRINTF) || defined(_WIN32)) && ! defined(DISABLE_VPRINTF)
    }
#endif
} // ibis::column::logError

void ibis::column::logWarning(const char* event, const char* fmt, ...) const {
    if (ibis::gVerbose < 0)
	return;
    char tstr[28];
    ibis::util::getLocalTime(tstr);
    FILE* fptr = ibis::util::getLogFile();

    ibis::util::ioLock lock;
    fprintf(fptr, "%s\nWarning -- column[%s.%s](%s)::%s -- ",
	    tstr, (thePart!=0 ? thePart->name() : ""), m_name.c_str(),
	    ibis::TYPESTRING[(int)m_type], event);

#if (defined(HAVE_VPRINTF) || defined(_WIN32)) && ! defined(DISABLE_VPRINTF)
    va_list args;
    va_start(args, fmt);
    vfprintf(fptr, fmt, args);
    va_end(args);
#else
    fprintf(fptr, "%s ... ", fmt);
#endif
    if (errno != 0) {
	if (errno != ENOENT)
	    fprintf(fptr, " ... %s", strerror(errno));
	errno = 0;
    }
    fprintf(fptr, "\n");
    fflush(fptr);
} // ibis::column::logWarning

void ibis::column::logMessage(const char* event, const char* fmt, ...) const {
    FILE* fptr = ibis::util::getLogFile();
    ibis::util::ioLock lock;
#if defined(FASTBIT_TIMED_LOG)
    char tstr[28];
    ibis::util::getLocalTime(tstr);
    fprintf(fptr, "%s   ", tstr);
#endif
    fprintf(fptr, "column[%s.%s](%s)::%s -- ",
	   (thePart != 0 ? thePart->name() : ""),
	    m_name.c_str(), ibis::TYPESTRING[(int)m_type], event);
#if (defined(HAVE_VPRINTF) || defined(_WIN32)) && ! defined(DISABLE_VPRINTF)
    va_list args;
    va_start(args, fmt);
    vfprintf(fptr, fmt, args);
    va_end(args);
#else
    fprintf(fptr, "%s ...", fmt);
#endif
    fprintf(fptr, "\n");
    fflush(fptr);
} // ibis::column::logMessage

/// Load the index associated with the column.
/// @param opt This option is passed to ibis::index::create to be used if a
/// new index is to be created.
/// @param readall If this argument is greater than zero, all metadata and
/// bitmaps associated with an index is read into memory; otherwise only
/// the metadata about the index is loaded into memory.  The bitmaps
/// associated with an index can be read into memory as needed.
///
/// @note Accesses to this function are serialized through a write lock on
/// the column.
void ibis::column::loadIndex(const char* opt, int readall) const throw () {
    if (idx != 0 || thePart == 0 || thePart->nRows() == 0 ||
	thePart->currentDataDir() == 0)
	return;

    writeLock lock(this, "loadIndex");
    if (idx != 0 || thePart->nRows() == 0)
	return;

    ibis::index* tmp = idx;
    try { // if an index is not available, create one
	if (ibis::gVerbose > 7)
	    logMessage("loadIndex", "loading the index from %s",
		       thePart->currentDataDir());
	if (tmp == 0) {
	    tmp = ibis::index::create(this, thePart->currentDataDir(),
				      opt, readall);
	}
	if (tmp == 0) { // failed to create index
	    purgeIndexFile(); // remove any left over index file
	    const_cast<column*>(this)->m_bins = "noindex";
	    std::string key = thePart->name();
	    key += '.';
	    key += m_name;
	    key += ".disableIndexOnFailure";
	    if (ibis::gParameters().isTrue(key.c_str())) {
		// don't try to build index any more
		thePart->updateMetaData();
	    }
	    return;
	}

	if (tmp->getNRows()
#if defined(DELETE_INDEX_ON_SIZE_MISMATCH)
	    !=
#else
	    >
#endif
	    thePart->nRows()) {
	    if (ibis::gVerbose > 2)
		logMessage("loadIndex", "found an index with nRows=%lu, "
			   "but the data partition nRows=%lu, try to "
			   "recreate the index",
			   static_cast<unsigned long>(tmp->getNRows()),
			   static_cast<unsigned long>(thePart->nRows()));
	    delete tmp;
	    // create a brand new index from data in the current working
	    // directory
	    tmp = ibis::index::create(this, static_cast<const char*>(0), opt);
	    if (tmp != 0 && tmp->getNRows() != thePart->nRows()) {
		if (ibis::gVerbose > 0)
		    logWarning("loadIndex",
			       "created an index with nRows=%lu, "
			       "but the data partition nRows=%lu, "
			       "failed on retry!",
			       static_cast<unsigned long>(tmp->getNRows()),
			       static_cast<unsigned long>
			       (thePart->nRows()));
		delete tmp;
		purgeIndexFile();
	    }
	}
	if (tmp != 0) {
	    if (ibis::gVerbose > 10) {
		ibis::util::logger lg;
		tmp->print(lg.buffer());
	    }

	    mutexLock lck2(this, "loadIndex");
	    if (idx == 0) {
		idx = tmp;
	    }
	    else { // somehow another thread has created an index
		LOGGER(ibis::gVerbose >= 0)
		    << "column[" << (thePart ? thePart->name() : "?") << '.'
		    << m_name << "]::loadIndex found an index (" << idx->name()
		    << ") for this column after building another one ("
		    << tmp->name() << "), discarding the new one";
		delete tmp;
	    }
	}
    }
    catch (const char* s) {
	logWarning("loadIndex", "index::ceate(%s) throw "
		   "the following exception\n%s", name(), s);
	delete tmp;
// 	    std::string key = thePart->name();
// 	    key += '.';
// 	    key += m_name;
// 	    key += ".disableIndexOnFailure";
// 	    if (ibis::gParameters().isTrue(key.c_str())) {
// 		// don't try to build index any more
// 		const_cast<column*>(this)->m_bins = "noindex";
// 		thePart->updateMetaData();
// 	    }
// 	    purgeIndexFile();
    }
    catch (const std::exception& e) {
	logWarning("loadIndex", "index::create(%s) failed "
		   "to create a new index -- %s", name(), e.what());
	delete tmp;
    }
    catch (...) {
	logWarning("loadIndex", "index::create(%s) failed "
		   "to create a new index -- unexpected exception",
		   name());
	delete tmp;
    }
} // ibis::column::loadIndex

/// Unload the index associated with the column.
// This function requires a write lock just like loadIndex.
void ibis::column::unloadIndex() const {
    mutexLock mlck(this, "unloadIndex");
    if (0 != idx) {
	softWriteLock lock(this, "unloadIndex");
	if (lock.isLocked() && 0 != idx) {
	    const uint32_t idxc = idxcnt();
	    if (0 == idxc) {
		delete idx;
		idx = 0;
		if (ibis::gVerbose > 7)
		    logMessage("unloadIndex", "successfully removed the index");
	    }
	    else {
		LOGGER(ibis::gVerbose > 0)
		    << "Warning -- ibis::column[" << thePart->name()
		    << "." << name() << "]::unloadIndex failed because "
		    "idxcnt (" << idxc << ") is not zero";
	    }
	}
    }
} // ibis::column::unloadIndex

void ibis::column::preferredBounds(std::vector<double>& tmp) const {
    indexLock lock(this, "preferredBounds");
    if (idx != 0) {
	idx->binBoundaries(tmp);
	if (tmp.back() == DBL_MAX) // remove the last element
	    tmp.resize(tmp.size()-1);
    }
    else {
	tmp.clear();
    }
} // ibis::column::preferredBounds

void ibis::column::binWeights(std::vector<uint32_t>& tmp) const {
    indexLock lock(this, "binWeights");
    if (idx != 0) {
	idx->binWeights(tmp);
    }
    else {
	tmp.clear();
    }
} // ibis::column::binWeights

/// Return a negative value if the index file does not exist.
long ibis::column::indexSize() const {
    std::string sname;
    if (dataFileName(sname) == 0) return -1;

    sname += ".idx";
    readLock lock(this, "indexSize");
    return ibis::util::getFileSize(sname.c_str());
} // ibis::column::indexSize

void ibis::column::indexSpeedTest() const {
    indexLock lock(this, "indexSpeedTest");
    if (idx != 0) {
	ibis::util::logger lg;
	idx->speedTest(lg.buffer());
    }
} // ibis::column::indexSpeedTest

void ibis::column::purgeIndexFile(const char *dir) const {
    if (dir == 0 && (thePart == 0 || thePart->currentDataDir() == 0))
	return;
    std::string fnm = (dir ? dir : thePart != 0 ? thePart->currentDataDir()
		       : ".");
    if (fnm[fnm.size()-1] != FASTBIT_DIRSEP)
	fnm += FASTBIT_DIRSEP;
    fnm += m_name;
    const unsigned len = fnm.size() + 1;
    fnm += ".idx";
    remove(fnm.c_str());
    fnm.erase(len);
    fnm += "bin";
    remove(fnm.c_str());
    if (m_type == ibis::TEXT) {
	fnm.erase(len);
	fnm += "terms";
	remove(fnm.c_str());
	fnm.erase(len);
	// 	fnm += "sp";
	// 	remove(fnm.c_str());
    }
    else if (m_type == ibis::CATEGORY) {
	fnm.erase(fnm.size() - 3);
	fnm += "dic";
	remove(fnm.c_str());
    }
} // ibis::column::purgeIndexFile

// expand range condition so that the boundaris are on the bin boundaries
int ibis::column::expandRange(ibis::qContinuousRange& rng) const {
    int ret = 0;
    indexLock lock(this, "expandRange");
    if (idx != 0) {
	ret = idx->expandRange(rng);
    }
    return ret;
} // ibis::column::expandRange

// expand range condition so that the boundaris are on the bin boundaries
int ibis::column::contractRange(ibis::qContinuousRange& rng) const {
    int ret = 0;
    indexLock lock(this, "contractRange");
    if (idx != 0) {
	ret = idx->contractRange(rng);
    }
    return ret;
} // ibis::column::contractRange

long ibis::column::evaluateRange(const ibis::qContinuousRange& cmp,
				 const ibis::bitvector& mask,
				 ibis::bitvector& low) const {
    long ierr = 0;
    ibis::bitvector mymask;
    getNullMask(mymask);
    mymask &= mask;
    low.clear(); // clear the existing content
    if (m_type == ibis::OID || m_type == ibis::TEXT) {
	LOGGER(ibis::gVerbose >= 0)
	    << "Warning -- column[" << thePart->name() << "." << m_name
	    << "]::evaluateRange(" << cmp
	    << ") -- the range condition is not applicable on the column type "
	    << ibis::TYPESTRING[(int)m_type];
	ierr = -4;
	return ierr;
    }

    try {
	ibis::bitvector high;
	{ // use a block to limit the scope of index lock
	    indexLock lock(this, "evaluateRange");
	    if (idx != 0) {
		double cost = idx->estimateCost(cmp);
		// use index only if the cost of using its estimate cost is
		// less than N bytes
		if (cost < thePart->nRows() * 0.5) {
		    idx->estimate(cmp, low, high);
		}
		else if (m_sorted) {
		    ierr = searchSorted(cmp, low);
		    if (ierr < 0)
			low.clear();
		}
	    }
	    else if (m_sorted) {
		ierr = searchSorted(cmp, low);
		if (ierr < 0)
		    low.clear();
	    }
	}
	if (low.size() != mymask.size()) { // short index
	    if (high.size() != low.size())
		high.copy(low);
	    high.adjustSize(mymask.size(), mymask.size());
	    low.adjustSize(0, mymask.size());
	}
	low &= mymask;
	if (low.size() == high.size()) { // need scan
	    ibis::bitvector b2;
	    high &= mymask;
	    high -= low;
	    if (high.cnt() > 0) {
		ierr = thePart->doScan(cmp, high, b2);
		if (ierr >= 0) {
		    low |= b2;
		}
		else {
		    low.clear();
		}
	    }
	}
	if (low.size() == mymask.size())
	    ierr = low.cnt();

	LOGGER(ibis::gVerbose > 3)
	    << "column[" << thePart->name() << "." << name()
	    << "]::evaluateRange(" << cmp << ", mask(" << mask.cnt()
	    << ", " << mask.size() << ")) completed with low.size() = "
	    << low.size() << ", low.cnt() = " << low.cnt()
	    << ", and ierr = " << ierr;
	return ierr;
    }
    catch (std::exception &se) {
	logWarning("evaluateRange", "received a std::exception -- %s",
		   se.what());
    }
    catch (const char* str) {
	logWarning("evaluateRange", "received a string exception -- %s",
		   str);
    }
    catch (...) {
	logWarning("evaluateRange", "received a unanticipated excetpion");
    }

    // Common exception handling -- retry the basic option of scanning data
    low.clear();
    unloadIndex();
    //purgeIndexFile();
    if (thePart != 0) {
	try {
	    ierr = thePart->doScan(cmp, mymask, low);
	}
	catch (...) {
	    LOGGER(ibis::gVerbose > 1)
		<< "column[" << thePart->name() << "." << name()
		<< "]::evaluateRange(" << cmp << ") receied an "
		"exception from doScan in the exception handling code, "
		"giving up...";
	    low.clear();
	    ierr = -2;
	}
    }
    else {
	LOGGER(ibis::gVerbose > 0)
	    << "Warning -- column[" << m_name
	    << "] does not belong to a data partition, but need one";
	ierr = -3;
    }

    LOGGER(ibis::gVerbose > 3)
	<< "column[" << thePart->name() << "." << name()
	<< "]::evaluateRange(" << cmp
	<< ") completed the fallback option with low.size() = "
	<< low.size() << ", low.cnt() = " << low.cnt()
	<< ", and ierr = " << ierr;
    return ierr;
} // ibis::column::evaluateRange

long ibis::column::evaluateRange(const ibis::qDiscreteRange& cmp,
				 const ibis::bitvector& mask,
				 ibis::bitvector& low) const {
    long ierr = -1;
    if (cmp.getValues().empty()) {
	low.set(0, mask.size());
	return 0;
    }
    if (m_type == ibis::OID || m_type == ibis::TEXT) {
	LOGGER(ibis::gVerbose >= 0)
	    << "Warning -- column[" << thePart->name() << "." << m_name
	    << "]::evaluateRange(" << cmp.colName() << " IN ...) -- "
	    << "the range condition is not applicable on the column type "
	    << ibis::TYPESTRING[(int)m_type];
	ierr = -4;
	return ierr;
    }
    if (m_type != ibis::FLOAT && m_type != ibis::DOUBLE &&
	cmp.getValues().size() ==
	1+(cmp.getValues().back()-cmp.getValues().front())) {
	// a special case -- actually a continuous range
	ibis::qContinuousRange cr(cmp.getValues().front(), ibis::qExpr::OP_LE,
				  cmp.colName(), ibis::qExpr::OP_LE,
				  cmp.getValues().back());
	return evaluateRange(cr, mask, low);
    }

    ibis::bitvector mymask;
    getNullMask(mymask);
    mymask &= mask;
    try {
	indexLock lock(this, "evaluateRange");
	if (idx != 0) {
	    double idxcost = idx->estimateCost(cmp) *
		(1.0 + std::log((double)cmp.getValues().size()));
	    if (m_sorted && idxcost > mymask.size()) {
		ierr = searchSorted(cmp, low);
		if (ierr == 0) {
		    low &= mymask;
		    return 0L;
		}
	    }
	    if (idxcost > (elementSize()+4.0) * mymask.size() &&
		thePart->currentDataDir() != 0) {
		// using a sorted list may be faster
		ibis::roster ros(this);
		if (ros.size() == thePart->nRows()) {
		    ierr = ros.locate(cmp.getValues(), low);
		    if (ierr >= 0) {
			low &= mymask;
			return 0L;
		    }
		}
	    }

	    // fall back to the normal indexing option
	    ierr = idx->evaluate(cmp, low);
	    if (ierr >= 0) {
		if (low.size() < mymask.size()) { // short index, scan
		    ibis::bitvector b1, b2;
		    b1.appendFill(0, low.size());
		    b1.appendFill(1, mymask.size()-low.size());
		    ierr = thePart->doScan(cmp, b1, b2);
		    if (ierr >= 0) {
			low.adjustSize(0, mymask.size());
			low |= b2;
		    }
		}
		low &= mymask;
	    }
	    else { // index::evaluate failed
#if defined(DEBUG) || defined(_DEBUG)
		LOGGER(ibis::gVerbose > 2)
		    << "INFO -- ibis::column[" << thePart->name()
		    << "." << name() << "]::evaluateRange(" << cmp.colName()
		    << " IN ...) -- idx(" << idx->name()
		    << ")->evaluate returned " << ierr
		    << ", attempting other alternatives";
#endif
		if (m_sorted) {
		    ierr = searchSorted(cmp, low);
		    if (ierr >= 0) {
			low &= mymask;
		    }
		}

		if (ierr < 0) {
		    ibis::bitvector high;
		    idx->estimate(cmp, low, high);
		    if (low.size() != mymask.size()) {
			if (high.size() == low.size()) {
			    high.adjustSize(mymask.size(), mymask.size());
			}
			else if (high.size() == 0) {
			    high.copy(low);
			    high.adjustSize(mymask.size(), mymask.size());
			}
			low.adjustSize(0, mymask.size());
		    }
		    low &= mymask;
		    if (high.size() == low.size()) {
			high &= mymask;
			if (high.cnt() > low.cnt()) {
			    high -= low;
			    ibis::bitvector delta;
			    if (high.cnt() > 0 && thePart != 0) {
				ierr = thePart->doScan(cmp, high, delta);
				if (ierr >= 0)
				    low |= delta;
			    }
			}
		    }
		}
	    }
	}
	else { // no index
	    if (m_sorted) {
		ierr = searchSorted(cmp, low);
	    }
	    else {
		ierr = -1;
	    }
	    if (ierr < 0 && thePart != 0) {
		ibis::roster ros(this);
		if (ros.size() == thePart->nRows()) {
		    ierr = ros.locate(cmp.getValues(), low);
		    if (ierr >= 0) {
			low &= mymask;
		    }
		}
		if (ierr < 0)
		    ierr = thePart->doScan(cmp, mymask, low);
	    }
	}

	LOGGER(ibis::gVerbose > 3)
	    << "column[" << thePart->name() << "." << name()
	    << "]::evaluateRange(" << cmp.colName() << " IN ...) "
	    << "completed with low.size() = " << low.size()
	    << ", low.cnt() = " << low.cnt() << ", and ierr = " << ierr;
	return ierr;
    }
    catch (std::exception &se) {
	logWarning("evaluateRange", "received a std::exception -- %s",
		   se.what());
    }
    catch (const char* str) {
	logWarning("evaluateRange", "received a string exception -- %s",
		   str);
    }
    catch (...) {
	logWarning("evaluateRange", "received a unanticipated excetpion");
    }

    // Common exception handling -- retry the basic option of scanning data
    low.clear();
    unloadIndex();
    //purgeIndexFile();
    if (thePart != 0) {
	try {
	    ierr = thePart->doScan(cmp, mymask, low);
	}
	catch (...) {
	    LOGGER(ibis::gVerbose > 1)
		<< "column[" << thePart->name() << "." << name()
		<< "]::evaluateRange(" << cmp.colName() << "IN ...) receied "
		"an exception from doScan in the exception handling code, "
		"giving up...";
	    low.clear();
	    ierr = -2;
	}
    }
    else {
	ierr = -3;
    }

    LOGGER(ibis::gVerbose > 3)
	<< "column[" << thePart->name() << "." << name()
	<< "]::evaluateRange(" << cmp.colName() << " IN ...) "
	<< "completed the fallback option with low.size() = "
	<< low.size() << ", low.cnt() = " << low.cnt()
	<< ", and ierr = " << ierr;
    return ierr;
} // ibis::column::evaluateRange

// use the index to generate the hit list and the candidate list
long ibis::column::estimateRange(const ibis::qContinuousRange& cmp,
				 ibis::bitvector& low,
				 ibis::bitvector& high) const {
    long ierr = 0;
    try {
	indexLock lock(this, "estimateRange");
	if (idx != 0) {
	    idx->estimate(cmp, low, high);
	    if (low.size() != thePart->nRows()) {
		if (high.size() == low.size()) {
		    high.adjustSize(thePart->nRows(), thePart->nRows());
		}
		else if (high.size() == 0) {
		    high.copy(low);
		    high.adjustSize(thePart->nRows(), thePart->nRows());
		}
		low.adjustSize(0, thePart->nRows());
	    }
	}
	else if (thePart != 0) {
	    low.set(0, thePart->nRows());
	    getNullMask(high);
	}
	else {
	    ierr = -1;
	}

	LOGGER(ibis::gVerbose > 4)
	    << "column[" << thePart->name() << "." << name()
	    << "]::estimateRange(" << cmp
	    << ") completed with low.size() = " << low.size()
	    << ", low.cnt() = " << low.cnt() << ", high.size() = "
	    << high.size() << ", high.cnt() = " << high.cnt()
	    << ", and ierr = " << ierr;
	return ierr;
    }
    catch (std::exception &se) {
	logWarning("estimateRange", "received a std::exception -- %s",
		   se.what());
    }
    catch (const char* str) {
	logWarning("estimateRange", "received a string exception -- %s",
		   str);
    }
    catch (...) {
	logWarning("estimateRange", "received a unanticipated excetpion");
    }

    // Common exception handling -- no estimate can be provided
    unloadIndex();
    //purgeIndexFile();
    if (thePart != 0) {
	low.set(0, thePart->nRows());
	getNullMask(high);
    }
    else {
	ierr = -2;
    }
    return -ierr;
} // ibis::column::estimateRange

// use the index to compute a maximum hit
long ibis::column::estimateRange(const ibis::qContinuousRange& cmp) const {
    long ret = (thePart != 0 ? thePart->nRows() : LONG_MAX);
    try {
	indexLock lock(this, "estimateRange");
	if (idx != 0)
	    ret = idx->estimate(cmp);
	return ret;
    }
    catch (std::exception &se) {
	logWarning("estimateRange", "received a std::exception -- %s",
		   se.what());
    }
    catch (const char* str) {
	logWarning("estimateRange", "received a string exception -- %s",
		   str);
    }
    catch (...) {
	logWarning("estimateRange", "received a unanticipated excetpion");
    }

    unloadIndex();
    //purgeIndexFile();
    return ret;
} // ibis::column::estimateRange

/// Estimating hits for a discrete range is actually done with
/// evaluateRange.
long ibis::column::estimateRange(const ibis::qDiscreteRange& cmp,
				 ibis::bitvector& low,
				 ibis::bitvector& high) const {
    high.clear();
    return evaluateRange(cmp, thePart->getNullMask(), low);
} // ibis::column::estimateRange

double ibis::column::estimateCost(const ibis::qContinuousRange& cmp) const {
    double ret;
    indexLock lock(this, "estimateCost");
    if (idx != 0)
	ret = idx->estimateCost(cmp);
    else
	ret = static_cast<double>(thePart != 0 ? thePart->nRows() :
				  0xFFFFFFFFU) * elementSize();
    return ret;
} // ibis::column::estimateCost

double ibis::column::estimateCost(const ibis::qDiscreteRange& cmp) const {
    double ret;
    indexLock lock(this, "estimateCost");
    if (idx != 0)
	ret = idx->estimateCost(cmp);
    else
	ret = static_cast<double>(thePart != 0 ? thePart->nRows() :
				  0xFFFFFFFFU) * elementSize();
    return ret;
} // ibis::column::estimateCost

/// Compute the locations of the rows can not be decided by the index.
/// Returns the fraction of rows might satisfy the specified range
/// condition.  If no index, nothing can be decided.
float ibis::column::getUndecidable(const ibis::qContinuousRange& cmp,
				   ibis::bitvector& iffy) const {
    float ret = 1.0;
    try {
	indexLock lock(this, "getUndecidable");
	if (idx != 0) {
	    ret = idx->undecidable(cmp, iffy);
	}
	else {
	    getNullMask(iffy);
	    ret = 1.0; // everything might satisfy the condition
	}
	return ret; // normal return
    }
    catch (std::exception &se) {
	logWarning("getUndecidable", "received a std::exception -- %s",
		   se.what());
	ret = 1.0; // everything is undecidable by index
    }
    catch (const char* str) {
	logWarning("getUndecidable", "received a string exception -- %s",
		   str);
	ret = 1.0;
    }
    catch (...) {
	logWarning("getUndecidable", "received a unanticipated excetpion");
	ret = 1.0;
    }

    unloadIndex();
    //purgeIndexFile();
    getNullMask(iffy);
    return ret;
} // ibis::column::getUndecidable

// use the index to compute a upper bound on the number of hits
long ibis::column::estimateRange(const ibis::qDiscreteRange& cmp) const {
    long ret = (thePart != 0 ? thePart->nRows() : LONG_MAX);
    try {
	indexLock lock(this, "estimateRange");
	if (idx != 0)
	    ret = idx->estimate(cmp);
	return ret;
    }
    catch (std::exception &se) {
	logWarning("estimateRange", "received a std::exception -- %s",
		   se.what());
    }
    catch (const char* str) {
	logWarning("estimateRange", "received a string exception -- %s",
		   str);
    }
    catch (...) {
	logWarning("estimateRange", "received a unanticipated excetpion");
    }

    unloadIndex();
    //purgeIndexFile();
    return ret;
} // ibis::column::estimateRange

// compute the rows that can not be decided by the index, if no index,
// nothing can be decided.
float ibis::column::getUndecidable(const ibis::qDiscreteRange& cmp,
				   ibis::bitvector& iffy) const {
    float ret = 1.0;
    try {
	indexLock lock(this, "getUndecidable");
	if (idx != 0) {
	    ret = idx->undecidable(cmp, iffy);
	}
	else {
	    getNullMask(iffy);
	    ret = 1.0; // everything might satisfy the condition
	}
	return ret; // normal return
    }
    catch (std::exception &se) {
	logWarning("getUndecidable", "received a std::exception -- %s",
		   se.what());
	ret = 1.0; // everything is undecidable by index
    }
    catch (const char* str) {
	logWarning("getUndecidable", "received a string exception -- %s",
		   str);
	ret = 1.0;
    }
    catch (...) {
	logWarning("getUndecidable", "received a unanticipated excetpion");
	ret = 1.0;
    }

    unloadIndex();
    //purgeIndexFile();
    getNullMask(iffy);
    return ret;
} // ibis::column::getUndecidable

/// Append the content of file in @c df to end of file in @c dt.  It
/// returns the number of rows appended or a negative number to indicate
/// error.
///
/// @note This function does not update the mininimum and the maximum of
/// the column.
long ibis::column::append(const char* dt, const char* df,
			  const uint32_t nold, const uint32_t nnew,
			  const uint32_t nbuf, char* buf) {
    long ret = 0;
    if (nnew == 0 || dt == 0 || df == 0 || *dt == 0 || *df == 0 ||
	strcmp(dt, df) == 0)
	return ret;
    std::string evt = "column[";
    if (thePart != 0)
	evt += thePart->name();
    else
	evt += "?";
    evt += '.';
    evt += m_name;
    evt += "]::append";
    int elem = elementSize();
    if (elem <= 0) {
	LOGGER(ibis::gVerbose >= 0)
	    << "Warning -- " << evt
	    << " unable to continue, elementSize() is not a positive number";
	return -1;
    }
    else if (static_cast<uint64_t>((nold+nnew))*elem >= 0x80000000LU) {
	LOGGER(ibis::gVerbose >= 0)
	    << "Warning -- " << evt
	    << " -- the new data file will have more than 2GB, nold="
	    << nold << ", nnew=" << nnew << ", elementSize()=" << elem;
	return -2;
    }

    writeLock lock(this, evt.c_str());
    std::string to;
    std::string from;
    to += dt;
    to += FASTBIT_DIRSEP;
    to += m_name;
    from += df;
    from += FASTBIT_DIRSEP;
    from += m_name;
    LOGGER(ibis::gVerbose > 3)
	<< evt << " -- source \"" << from << "\" --> destination \""
	<< to << "\", nold=" << nold << ", nnew=" << nnew;

    // open destination file, position the file pointer
    int dest = UnixOpen(to.c_str(), OPEN_APPENDONLY, OPEN_FILEMODE);
    if (dest < 0) {
	logWarning("append", "unable to open file \"%s\" for append ... %s",
		   to.c_str(),
		   (errno ? strerror(errno) : "no free stdio stream"));
	return -3;
    }
#if defined(_WIN32) && defined(_MSC_VER)
    (void)_setmode(dest, _O_BINARY);
#endif
    uint32_t j = UnixSeek(dest, 0, SEEK_END);
    uint32_t sz = elem*nold, nnew0 = 0;
    uint32_t nold0 = j / elem;
    if (nold > nold0) { // existing destination smaller than expected
	memset(buf, 0, nbuf);
	while (j < sz) {
	    uint32_t diff = sz - j;
	    if (diff > nbuf)
		diff = nbuf;
	    UnixWrite(dest, buf, diff);
	    j += diff;
	}
    }
    if (UnixSeek(dest, sz, SEEK_SET) < static_cast<long>(sz)) {
	// can not move file pointer to the expected location
	UnixClose(dest);
	logWarning("append", "failed to seek to %ld in %s",
		   static_cast<long>(sz), to.c_str());
	return -4;
    }

    ret = 0;	// to count the number of bytes written
    int src = UnixOpen(from.c_str(), OPEN_READONLY); // open the files
    if (src >= 0) { // open the source file, copy it
#if defined(_WIN32) && defined(_MSC_VER)
	(void)_setmode(src, _O_BINARY);
#endif
	const uint32_t tgt = nnew * elem;
	long iread=1, iwrite;
	while (static_cast<uint32_t>(ret) < tgt &&
	       (iread = UnixRead(src, buf, nbuf)) > 0) {
	    if (iread + ret > static_cast<long>(tgt)) {
		// write at most tgt bytes
		LOGGER(ibis::gVerbose > 1)
		    << evt << " -- read " << iread << " bytes from " << from
		    << ", but expected " << (tgt-ret) << ", will use first "
		    << (tgt-ret) << " bytes";
		iread = tgt - ret;
	    }
	    iwrite = UnixWrite(dest, buf, iread);
	    if (iwrite != iread) {
		logWarning("append", "Only wrote %ld out of %ld bytes to "
			   "\"%s\" after written %ld elements",
			   static_cast<long>(iwrite), static_cast<long>(iread),
			   to.c_str(), ret);
	    }
	    ret += (iwrite>0 ? iwrite : 0);
	}
	UnixClose(src);
	m_sorted = false; // assume no longer sorted
	LOGGER(ibis::gVerbose > 8)
	    << evt << " -- copied " << ret << " bytes from \"" << from
	    << "\" to \"" << to << "\"";
    }
    else if (ibis::gVerbose > 0) { // can not open source file, write 0
	logWarning("append", "unable to open file \"%s\" for reading ... "
		   "%s\nwill write zeros in its place",
		   from.c_str(),
		   (errno ? strerror(errno) : "no free stdio stream"));
    }
    j = UnixSeek(dest, 0, SEEK_CUR);
    sz = elem * (nold + nnew);
    nnew0 = (j / elem) - nold;
    if (j < sz) {
	memset(buf, 0, nbuf);
	while (j < sz) {
	    uint32_t diff = sz - j;
	    if (diff > nbuf)
		diff = nbuf;
	    UnixWrite(dest, buf, diff);
	    j += diff;
	}
    }
#if _POSIX_FSYNC+0 > 0 && defined(FASTBIT_SYNC_WRITE)
    (void) UnixFlush(dest); // write to disk
#endif
    UnixClose(dest);
    if (j != sz) {
	logWarning("append", "file \"%s\" size (%ld) differs from the "
		   "expected value %ld", to.c_str(),
		   static_cast<long>(j), static_cast<long>(sz));
	if (j > sz) //truncate the file to the expected size
	    truncate(to.c_str(), sz);
    }
    else if (ibis::gVerbose > 10) {
	logMessage("append", "size of \"%s\" is %lu as expected", to.c_str(),
		   static_cast<long unsigned>(j));
    }

    ret /= elem;	// convert to the number of elements written
    LOGGER(ibis::gVerbose > 4)
	<< evt << " appended " << ret << " row" << (ret>1?"s":"");
    if (m_type == ibis::OID) {
	return ret;
    }

    //////////////////////////////////////////////////
    // deals with the masks
    std::string filename;
    filename = from;
    filename += ".msk";
    ibis::bitvector mapp;
    try {mapp.read(filename.c_str());} catch (...) {/* ok to continue */}
    mapp.adjustSize(nnew0, nnew);
    LOGGER(ibis::gVerbose > 7)
	<< evt << " mask file \"" << filename << "\" contains "
	<< mapp.cnt() << " set bits out of " << mapp.size()
	<< " total bits";

    filename = to;
    filename += ".msk";
    ibis::bitvector mtot;
    try {mtot.read(filename.c_str());} catch (...) {/* ok to continue */}
    mtot.adjustSize(nold0, nold);
    LOGGER(ibis::gVerbose > 7)
	<< evt << " mask file \"" << filename << "\" contains " << mtot.cnt()
	<< " set bits out of " << mtot.size() << " total bits before append";

    mtot += mapp; // append the new ones at the end
    if (mtot.size() != nold+nnew) {
	if (ibis::gVerbose > 0)
	    logWarning("append", "combined mask (%lu-bits) is expected to "
		       "have %lu bits, but it is not.  Will force it to "
		       "the expected size",
		       static_cast<long unsigned>(mtot.size()),
		       static_cast<long unsigned>(nold+nnew));
	mtot.adjustSize(nold+nnew, nold+nnew);
    }
    if (mtot.cnt() != mtot.size()) {
	mtot.write(filename.c_str());
	if (ibis::gVerbose > 6) {
	    logMessage("append", "mask file \"%s\" indicates %lu valid "
		       "records out of %lu", filename.c_str(),
		       static_cast<long unsigned>(mtot.cnt()),
		       static_cast<long unsigned>(mtot.size()));
#if defined(DEBUG)
	    LOGGER(ibis::gVerbose >= 0) << mtot;
#endif
	}
    }
    else {
	remove(filename.c_str()); // no need to have the file
	if (ibis::gVerbose > 6)
	    logMessage("append", "mask file \"%s\" removed, all "
		       "%lu records are valid", filename.c_str(),
		       static_cast<long unsigned>(mtot.size()));
    }
    if (thePart == 0 || thePart->currentDataDir() == 0)
	return ret;
    if (strcmp(dt, thePart->currentDataDir()) == 0) {
	// update the mask stored internally
	mutexLock lck(this, "column::append");
	mask_.swap(mtot);
    }

    //////////////////////////////////////////////////
    // deal with the index
    ibis::index* ind = 0;
    j = filename.size();
    filename[--j] = 'x'; // msk --> idx
    filename[--j] = 'd';
    filename[--j] = 'i';
    j = ibis::util::getFileSize(filename.c_str());
    if (thePart->getState() == ibis::part::TRANSITION_STATE) {
	if (thePart->currentDataDir() != 0) {
	    // the active directory may have up to date indices
	    std::string ff = thePart->currentDataDir();
	    ff += FASTBIT_DIRSEP;
	    ff += m_name;
	    ff += ".idx";
	    Stat_T st;
	    if (UnixStat(ff.c_str(), &st) == 0) {
		if (st.st_atime >= thePart->timestamp()) {
		    // copy the fresh index file
		    ibis::util::copy(filename.c_str(), ff.c_str());
		    if (ibis::gVerbose > 6)
			logMessage("append",
				   "copied index file \"%s\" to \"%s\"",
				   ff.c_str(), filename.c_str());
		}
		else if (j > 0) { // remove the stale file
		    remove(filename.c_str());
		}
	    }
	    else if (j > 0) { // remove the stale index file
		remove(filename.c_str());
	    }
	}
    }
    else if (thePart->nRows() > 0) {
	if (j > 0) { // the idx file exists
	    ind = ibis::index::create(this, dt);
	    if (ind && ind->getNRows() == nold) {
		// existing file maps successfully into an index
		long ierr = ind->append(dt, df, nnew);
		// the append operation have forced the index into memory,
		// remove record of the old index file
		ibis::fileManager::instance().flushFile(filename.c_str());
		if (static_cast<uint32_t>(ierr) == nnew) { // success
		    ind->write(dt);	// record the updated index
		    if (ibis::gVerbose > 6)
			logMessage("append", "successfully extended the "
				   "index in %s", dt);
		    if (ibis::gVerbose > 8) {
			ibis::util::logger lg;
			ind->print(lg.buffer());
		    }
		    delete ind;
		}
		else {			// failed to append
		    delete ind;
		    remove(filename.c_str());
		    if (ibis::gVerbose > 4)
			logMessage("append", "failed to extend the index "
				   "(code: %ld), removing file \"%s\"",
				   ierr, filename.c_str());
		}
	    }
#ifdef APPEND_UPDATE_INDEXES
	    else { // directly create the new indices
		ind = ibis::index::create(this, dt);
		if (ind != 0 && ibis::gVerbose > 6)
		    logMessage("append", "successfully created the "
			       "index in %s", dt);
		if (ibis::gVerbose > 8) {
		    ibis::util::logger lg;
		    ind->print(lg.buffer());
		}
		delete ind;
		ind = ibis::index::create(this, df);
		if (ind != 0 && ibis::gVerbose > 6)
		    logMessage("append", "successfully created the "
			       "index in %s", df);
		delete ind;
	    }
#else
	    else { // clean up the stale index
		delete ind;
		ibis::fileManager::instance().flushFile(filename.c_str());
		// simply remove the existing index file
		remove(filename.c_str());
	    }
#endif
	}
#ifdef APPEND_UPDATE_INDEXES
	else { // directly create the indices
	    ind = ibis::index::create(this, dt);
	    if (ind != 0 && ibis::gVerbose > 6)
		logMessage("append", "successfully created the "
			   "index in %s", dt);
	    if (ibis::gVerbose > 8) {
		ibis::util::logger lg;
		ind->print(lg.buffer());
	    }
	    delete ind;
	    ind = ibis::index::create(this, df);
	    if (ind != 0 && ibis::gVerbose > 6)
		logMessage("append", "successfully created the "
			   "index in %s", df);
	    delete ind;
	}
#endif
    }
#ifdef APPEND_UPDATE_INDEXES
    else { // dt and df contains the same data
	ind = ibis::index::create(this, dt);
	if (ind) {
	    if (ibis::gVerbose > 6)
		logMessage("append", "successfully created the "
			   "index in %s (also wrote to %s)", dt, df);
	    ind->write(df);
	    if (ibis::gVerbose > 8) {
		ibis::util::logger lg;
		ind->print(lg.buffer());
	    }
	    delete ind;
	}
    }
#endif
    return ret;
} // ibis::column::append

/// Convert string values in the opened file to a list of integers with the
/// aid of a dictionary.
/// - return 0 if there is no more elements in file.
/// - return a positive value if more bytes remain in the file.
/// - return a negative value if an error is encountered during the read
///   operation.
long ibis::column::string2int(int fptr, dictionary& dic,
			      uint32_t nbuf, char* buf,
			      array_t<uint32_t>& out) const {
    out.clear(); // clear the current integer list
    long ierr = 1;
    int32_t nread = UnixRead(fptr, buf, nbuf);
    ibis::fileManager::instance().recordPages(0, nread);
    if (nread <= 0) { // nothing is read, end-of-file or error ?
	if (nread == 0) {
	    ierr = 0;
	}
	else {
	    logWarning("string2int", "failed to read (read returned %ld)",
		       static_cast<long>(nread));
	    ierr = -1;
	}
	return ierr;
    }
    if (nread < static_cast<int32_t>(nbuf)) {
	// end-of-file, make sure the last string is terminated properly
	if (buf[nread-1]) {
	    buf[nread] = static_cast<char>(0);
	    ++ nread;
	}
    }

    const char* last = buf + nread;
    const char* endchar = buf;	// points to the next NULL character
    const char* str = buf;	// points to the next string

    while (endchar < last && *endchar != static_cast<char>(0)) ++ endchar;
    if (endchar >= last) {
	logWarning("string2int", "encountered a string longer than %ld bytes",
		   static_cast<long>(nread));
	return -2;
    }

    while (endchar < last) { // *endchar == 0
	uint32_t ui = dic.insert(str);
	out.push_back(ui);
	++ endchar; // skip over one NULL character
	str = endchar;
	while (endchar < last && *endchar != static_cast<char>(0)) ++ endchar;
    }

    if (endchar > str) { // need to move the file pointer backward
	long off = endchar - str;
	ierr = UnixSeek(fptr, -off, SEEK_CUR);
	if (ierr < 0) {
	    logWarning("string2int", "failed to move file pointer back %ld "
		       "bytes (ierr=%ld)", off, ierr);
	    ierr = -3;
	}
    }
    if (ierr >= 0)
	ierr = out.size();
    if (ibis::gVerbose > 4 && ierr >= 0)
	logMessage("string2int", "converted %ld string%s to integer%s",
		   ierr, (ierr>1?"s":""), (ierr>1?"s":""));
    return ierr;
} // ibis::column::string2int

/// Append the records in vals to the current working dataset.  The 'void*'
/// in this function follows the convention of the function getValuesArray
/// (not writeData), i.e., for the ten fixed-size elementary data types, it
/// is array_t<type>* and for string-valued columns it is
/// std::vector<std::string>*.
///
/// Return the number of entries actually written to disk or a negative
/// number to indicate error conditions.
long ibis::column::append(const void* vals, const ibis::bitvector& msk) {
    if (thePart == 0 || thePart->name() == 0 || thePart->currentDataDir() == 0)
	return -1L;
    if (m_name.empty()) return -2L;

    long ierr;
    writeLock lock(this, "appendValues");
    switch (m_type) {
    case ibis::BYTE:
	ierr = appendValues(* static_cast<const array_t<char>*>(vals), msk);
	break;
    case ibis::UBYTE:
	ierr = appendValues(* static_cast<const array_t<unsigned char>*>(vals),
			    msk);
	break;
    case ibis::SHORT:
	ierr = appendValues(* static_cast<const array_t<int16_t>*>(vals), msk);
	break;
    case ibis::USHORT:
	ierr = appendValues(* static_cast<const array_t<uint16_t>*>(vals), msk);
	break;
    case ibis::INT:
	ierr = appendValues(* static_cast<const array_t<int32_t>*>(vals), msk);
	break;
    case ibis::UINT:
	ierr = appendValues(* static_cast<const array_t<uint32_t>*>(vals), msk);
	break;
    case ibis::LONG:
	ierr = appendValues(* static_cast<const array_t<int64_t>*>(vals), msk);
	break;
    case ibis::ULONG:
	ierr = appendValues(* static_cast<const array_t<uint64_t>*>(vals), msk);
	break;
    case ibis::FLOAT:
	ierr = appendValues(* static_cast<const array_t<float>*>(vals), msk);
	break;
    case ibis::DOUBLE:
	ierr = appendValues(* static_cast<const array_t<double>*>(vals), msk);
	break;
    case ibis::CATEGORY:
    case ibis::TEXT:
	ierr = appendStrings
	    (* static_cast<const std::vector<std::string>*>(vals), msk);
	break;
    default:
	ierr = -3L;
	LOGGER(ibis::gVerbose >= 0)
	    << "Warning -- column[" << thePart->name() << '.' << m_name
	    << "]::append can not handle type " << (int) m_type << " ("
	    << ibis::TYPESTRING[(int)m_type] << ')';
	break;
    } // siwthc (m_type)
    return ierr;
} // ibis::column::append

/// This function attempts to fill the data file with NULL values if the
/// existing data file is shorter than expected.  It writes the data in
/// vals and extends the existing validity mask.
template <typename T>
long ibis::column::appendValues(const array_t<T>& vals,
				const ibis::bitvector& msk) {
    std::string evt = "column[";
    evt += thePart->name();
    evt += '.';
    evt += m_name;
    evt += "]::appendValues<";
    evt += typeid(T).name();
    evt += '>';
    std::string fn = thePart->currentDataDir();
    fn += FASTBIT_DIRSEP;
    fn += m_name;
    int curr = UnixOpen(fn.c_str(), OPEN_APPENDONLY, OPEN_FILEMODE);
    if (curr < 0) {
	LOGGER(ibis::gVerbose >= 0)
	    << "Warning -- " << evt << " failed to open file " << fn
	    << " for writing -- " << (errno != 0 ? strerror(errno) : "??");
	return -5L;
    }

    long ierr = 0;
    const unsigned elem = sizeof(T);
    off_t oldsz = UnixSeek(curr, 0, SEEK_END);
    mutexLock lock(this, evt.c_str());
    if (oldsz < 0)
	oldsz = 0;
    else
	oldsz = oldsz / elem;
    if (static_cast<size_t>(oldsz) < thePart->nRows()) {
	mask_.adjustSize(oldsz, thePart->nRows());
	while (static_cast<size_t>(oldsz) < thePart->nRows()) {
	    const size_t nw = (thePart->nRows()-oldsz <= vals.size() ?
			       thePart->nRows()-oldsz : vals.size());
	    ierr = UnixWrite(curr, vals.begin(), nw*elem);
	    if (ierr < static_cast<long>(nw*elem)) {
		LOGGER(ibis::gVerbose >= 0)
		    << "Warning -- " << evt << " failed to write " << nw*elem
		    << " bytes to " << fn << ", the write function returned "
		    << ierr;
		UnixClose(curr);
		return -6L;
	    }
	}
    }
    else if (static_cast<size_t>(oldsz) > thePart->nRows()) {
	mask_.adjustSize(thePart->nRows(), thePart->nRows());
	UnixSeek(curr, elem * thePart->nRows(), SEEK_SET);
    }

    ierr = UnixWrite(curr, vals.begin(), vals.size()*elem);
    if (ierr < static_cast<long>(vals.size() * elem)) {
	LOGGER(ibis::gVerbose >= 0)
	    << "Warning -- " << evt << " failed to write " << vals.size()*elem
	    << " bytes to " << fn << ", the write function returned " << ierr;
	UnixClose(curr);
	return -7L;
    }

    UnixClose(curr);
    LOGGER(ibis::gVerbose > 2)
	<< evt << " successfully added " << vals.size() << " element"
	<< (vals.size()>1?"s":"") << " to " << fn;

    ierr = vals.size();
    mask_ += msk;
    mask_.adjustSize(thePart->nRows()+vals.size(),
		     thePart->nRows()+vals.size());
    if (mask_.cnt() < mask_.size()) {
	fn += ".msk";
	mask_.write(fn.c_str());
    }
    return ierr;
} // ibis::column::appendValues

/// This function attempts to fill the existing data file with null values
/// based on the content of the validity mask.   It then write strings in
/// vals and extends the validity mask.
long ibis::column::appendStrings(const std::vector<std::string>& vals,
				 const ibis::bitvector& msk) {
    std::string evt = "column[";
    evt += thePart->name();
    evt += '.';
    evt += m_name;
    evt += "]::appendStrings";

    mutexLock lock(this, evt.c_str());
    std::string fn = thePart->currentDataDir();
    fn += FASTBIT_DIRSEP;
    fn += m_name;
    int curr = UnixOpen(fn.c_str(), OPEN_APPENDONLY, OPEN_FILEMODE);
    if (curr < 0) {
	LOGGER(ibis::gVerbose >= 0)
	    << "Warning -- " << evt << " failed to open file " << fn
	    << " for writing -- " << (errno != 0 ? strerror(errno) : "??");
	return -5L;
    }

    long ierr = 0;
    if (mask_.size() < thePart->nRows()) {
	char tmp[128];
	for (unsigned j = 0; j < 128; ++ j)
	    tmp[j] = 0;
	for (unsigned j = mask_.size(); j < thePart->nRows(); j += 128) {
	    const long nw = (thePart->nRows()-j <= 128 ?
			     thePart->nRows()-j : 128);
	    ierr = UnixWrite(curr, tmp, nw);
	    if (ierr < nw) {
		LOGGER(ibis::gVerbose >= 0)
		    << "Warning -- " << evt << " failed to write " << nw
		    << " bytes to " << fn << ", the write function returned "
		    << ierr;
		UnixClose(curr);
		return -6L;
	    }
	}
	mask_.adjustSize(0, thePart->nRows());
    }

    for (ierr = 0; ierr < static_cast<long>(vals.size()); ++ ierr) {
	long jerr = UnixWrite(curr, vals[ierr].c_str(), 1+vals[ierr].size());
	if (jerr < static_cast<long>(1 + vals[ierr].size())) {
	    LOGGER(ibis::gVerbose >= 0)
		<< "Warning -- " << evt << " failed to write "
		<< 1+vals[ierr].size() << " bytes to " << fn
		<< ", the write function returned " << jerr;
	    UnixClose(curr);
	    return -7L;
	}
    }
    UnixClose(curr);
    LOGGER(ibis::gVerbose > 2)
	<< evt << " successfully added " << vals.size() << " string"
	<< (vals.size()>1?"s":"") << " to " << fn;
    mask_ += msk;
    mask_.adjustSize(thePart->nRows()+vals.size(),
		     thePart->nRows()+vals.size());
    if (mask_.cnt() < mask_.size()) {
	fn += ".msk";
	mask_.write(fn.c_str());
    }
    return ierr;
} // ibis::column::appendStrings

/// Write the content in array va1 to directory dir.  Extend the mask.
/// The void* is internally cast into a pointer to the fixed-size
/// elementary data types according to the type of column.  Therefore,
/// there is no way this function can handle string values.
/// - Normally: record the content in array va1 to the directory dir.
/// - Special case 1: the OID column writes the second array va2 only.
/// - Special case 2: for string values, va2 is recasted to be the number
///   of bytes in va1.
///
/// Return the number of entries actually written to file.  If writing was
/// completely successful, the return value should match nnew.  It also
/// extends the mask.  Write out the mask if not all the bits are set.
long ibis::column::writeData(const char *dir, uint32_t nold, uint32_t nnew,
			     ibis::bitvector& mask, const void *va1,
			     const void *va2) {
    long ierr = 0;
    if (dir == 0 || nnew  == 0 || va1 == 0) return ierr;

    uint32_t nact = 0;
    char fn[PATH_MAX];
    uint32_t ninfile=0;
    sprintf(fn, "%s%c%s", dir, FASTBIT_DIRSEP, m_name.c_str());
    FILE *fdat = fopen(fn, "ab");
    if (fdat == 0) {
	logWarning("writeData", "unable to open \"%s\" for writing ... %s", fn,
		   (errno ? strerror(errno) : "no free stdio stream"));
	return ierr;
    }

    // Part I: write the content of val
    ninfile = ftell(fdat);
    if (m_type == ibis::UINT) {
	const unsigned int tmp = 4294967295U;
	const unsigned int elem = sizeof(unsigned int);
	if (ninfile != nold*elem) {
	    logMessage("writeData", "file \"%s\" is expected to have %lu "
		       "bytes but it has %lu", fn,
		       static_cast<long unsigned>(nold*elem),
		       static_cast<long unsigned>(ninfile));
	    if (ninfile > (nold+nnew)*elem) {
		// need to truncate the file
		fclose(fdat);
		truncate(fn, (nold+nnew)*elem);
		fdat = fopen(fn, "ab");
	    }
	    else if (ninfile < nold*elem) {
		ninfile /= elem;
		for (uint32_t i = ninfile; i < nold; ++ i) {
		    // write NULL values
		    ierr = fwrite(&tmp, elem, 1, fdat);
		    if (ierr == 0)
			logError("writeData", "unable to write to \"%s\"",
				 fn);
		}
	    }
	    ierr = fseek(fdat, nold*elem, SEEK_SET);
	}
	if (ninfile > nold)
	    ninfile = nold;

	const unsigned int *arr = reinterpret_cast<const unsigned int*>(va1);
	unsigned int il, iu;
	il = arr[0];
	iu = arr[0];

	for (uint32_t i=1; i<nnew; ++i) {
	    if (arr[i] > iu) {
		iu = arr[i];
	    }
	    else if (arr[i] < il) {
		il = arr[i];
	    }
	}
	if (nold <= 0) {
	    lower = il;
	    upper = iu;
	}
	else {
	    if (lower > il) lower = il;
	    if (upper < iu) upper = iu;
	}

	nact = fwrite(arr, elem, nnew, fdat);
	fclose(fdat);
	if (nact < nnew ) {
	    logWarning("writeData", "expected to write %lu unsigned int "
		       "to \"%s\", but only wrote %lu",
		       static_cast<long unsigned>(nnew), fn,
		       static_cast<long unsigned>(nact));
	}
    }
    else if (m_type == ibis::INT) {
	// data type is int -- signed integers
	const int tmp = 2147483647;
	const unsigned int elem = sizeof(int);
	if (ninfile != nold*elem) {
	    logMessage("writeData", "file \"%s\" is expected to have %lu "
		       "bytes but it has %lu", fn,
		       static_cast<long unsigned>(nold*elem),
		       static_cast<long unsigned>(ninfile));
	    if (ninfile > (nold+nnew)*elem) {
		fclose(fdat);
		truncate(fn, (nold+nnew)*elem);
		fdat = fopen(fn, "ab");
	    }
	    else if (ninfile < nold*elem) {
		ninfile /= elem;
		for (uint32_t i = ninfile; i < nold; ++ i) {
		    // write NULLs
		    ierr = fwrite(&tmp, elem, 1, fdat);
		    if (ierr == 0)
			logError("writeData", "unable to write to \"%s\"",
				 fn);
		}
	    }
	    ierr = fseek(fdat, nold*elem, SEEK_SET);
	}
	if (ninfile > nold)
	    ninfile = nold;

	const int *arr = reinterpret_cast<const int*>(va1);
	int il, iu;
	il = arr[0];
	iu = arr[0];

	for (uint32_t i = 1; i < nnew; ++ i) {
	    if (arr[i] > iu) {
		iu = arr[i];
	    }
	    else if (arr[i] < il) {
		il = arr[i];
	    }
	}
	if (nold <= 0) {
	    lower = il;
	    upper = iu;
	}
	else {
	    if (lower > il) lower = il;
	    if (upper < iu) upper = iu;
	}

	nact = fwrite(arr, elem, nnew, fdat);
	fclose(fdat);
	if (nact < nnew ) {
	    logWarning("writeData", "expected to write %lu int "
		       "to \"%s\", but only wrote %lu",
		       static_cast<long unsigned>(nnew), fn,
		       static_cast<long unsigned>(nact));
	}
    }
    else if (m_type == ibis::USHORT) {
	// data type is unsigned 2-byte integer
	const unsigned short int tmp = 65535;
	const unsigned int elem = sizeof(unsigned short int);
	if (ninfile != nold*elem) {
	    logMessage("writeData", "file \"%s\" is expected to have %lu "
		       "bytes but it has %lu", fn,
		       static_cast<long unsigned>(nold*elem),
		       static_cast<long unsigned>(ninfile));
	    if (ninfile > (nold+nnew)*elem) {
		// need to truncate the file
		fclose(fdat);
		truncate(fn, (nold+nnew)*elem);
		fdat = fopen(fn, "ab");
	    }
	    else if (ninfile < nold*elem) {
		ninfile /= elem;
		for (uint32_t i = ninfile; i < nold; ++ i) {
		    // write NULL values
		    ierr = fwrite(&tmp, elem, 1, fdat);
		    if (ierr == 0)
			logError("writeData", "unable to write to \"%s\"",
				 fn);
		}
	    }
	    ierr = fseek(fdat, nold*elem, SEEK_SET);
	}
	if (ninfile > nold)
	    ninfile = nold;

	const unsigned short int *arr =
	    reinterpret_cast<const unsigned short int*>(va1);
	unsigned short int il, iu;
	il = arr[0];
	iu = arr[0];

	for (uint32_t i=1; i<nnew; ++i) {
	    if (arr[i] > iu) {
		iu = arr[i];
	    }
	    else if (arr[i] < il) {
		il = arr[i];
	    }
	}
	if (nold <= 0) {
	    lower = il;
	    upper = iu;
	}
	else {
	    if (lower > il) lower = il;
	    if (upper < iu) upper = iu;
	}

	nact = fwrite(arr, elem, nnew, fdat);
	fclose(fdat);
	if (nact < nnew ) {
	    logWarning("writeData", "expected to write %lu unsigned int "
		       "to \"%s\", but only wrote %lu",
		       static_cast<long unsigned>(nnew), fn,
		       static_cast<long unsigned>(nact));
	}
    }
    else if (m_type == ibis::SHORT) {
	// data type is int -- signed short (2-byte) integers
	const short int tmp = 32767;
	const unsigned int elem = sizeof(short int);
	if (ninfile != nold*elem) {
	    logMessage("writeData", "file \"%s\" is expected to have %lu "
		       "bytes but it has %lu", fn,
		       static_cast<long unsigned>(nold*elem),
		       static_cast<long unsigned>(ninfile));
	    if (ninfile > (nold+nnew)*elem) {
		fclose(fdat);
		truncate(fn, (nold+nnew)*elem);
		fdat = fopen(fn, "ab");
	    }
	    else if (ninfile < nold*elem) {
		ninfile /= elem;
		for (uint32_t i = ninfile; i < nold; ++ i) {
		    // write NULLs
		    ierr = fwrite(&tmp, elem, 1, fdat);
		    if (ierr == 0)
			logError("writeData", "unable to write to \"%s\"",
				 fn);
		}
	    }
	    ierr = fseek(fdat, nold*elem, SEEK_SET);
	}
	if (ninfile > nold)
	    ninfile = nold;

	const short int *arr = reinterpret_cast<const short int*>(va1);
	short int il, iu;
	il = arr[0];
	iu = arr[0];

	for (uint32_t i = 1; i < nnew; ++ i) {
	    if (arr[i] > iu) {
		iu = arr[i];
	    }
	    else if (arr[i] < il) {
		il = arr[i];
	    }
	}
	if (nold <= 0) {
	    lower = il;
	    upper = iu;
	}
	else {
	    if (lower > il) lower = il;
	    if (upper < iu) upper = iu;
	}

	nact = fwrite(arr, elem, nnew, fdat);
	fclose(fdat);
	if (nact < nnew ) {
	    logWarning("writeData", "expected to write %lu int "
		       "to \"%s\", but only wrote %lu",
		       static_cast<long unsigned>(nnew), fn,
		       static_cast<long unsigned>(nact));
	}
    }
    else if (m_type == ibis::UBYTE) {
	// data type is 1-byte integer
	const unsigned char tmp = 255;
	const unsigned int elem = sizeof(unsigned char);
	if (ninfile != nold*elem) {
	    logMessage("writeData", "file \"%s\" is expected to have %lu "
		       "bytes but it has %lu", fn,
		       static_cast<long unsigned>(nold*elem),
		       static_cast<long unsigned>(ninfile));
	    if (ninfile > (nold+nnew)*elem) {
		// need to truncate the file
		fclose(fdat);
		truncate(fn, (nold+nnew)*elem);
		fdat = fopen(fn, "ab");
	    }
	    else if (ninfile < nold*elem) {
		ninfile /= elem;
		for (uint32_t i = ninfile; i < nold; ++ i) {
		    // write NULL values
		    ierr = fwrite(&tmp, elem, 1, fdat);
		    if (ierr == 0)
			logError("writeData", "unable to write to \"%s\"",
				 fn);
		}
	    }
	    ierr = fseek(fdat, nold*elem, SEEK_SET);
	}
	if (ninfile > nold)
	    ninfile = nold;

	const unsigned char *arr = reinterpret_cast<const unsigned char*>(va1);
	unsigned char il, iu;
	il = arr[0];
	iu = arr[0];

	for (uint32_t i=1; i<nnew; ++i) {
	    if (arr[i] > iu) {
		iu = arr[i];
	    }
	    else if (arr[i] < il) {
		il = arr[i];
	    }
	}
	if (nold <= 0) {
	    lower = il;
	    upper = iu;
	}
	else {
	    if (lower > il) lower = il;
	    if (upper < iu) upper = iu;
	}

	nact = fwrite(arr, elem, nnew, fdat);
	fclose(fdat);
	if (nact < nnew ) {
	    logWarning("writeData", "expected to write %lu unsigned int "
		       "to \"%s\", but only wrote %lu",
		       static_cast<long unsigned>(nnew), fn,
		       static_cast<long unsigned>(nact));
	}
    }
    else if (m_type == ibis::BYTE) {
	// data type is 1-byte signed integers
	const signed char tmp = 127;
	const unsigned int elem = sizeof(signed char);
	if (ninfile != nold*elem) {
	    logMessage("writeData", "file \"%s\" is expected to have %lu "
		       "bytes but it has %lu", fn,
		       static_cast<long unsigned>(nold*elem),
		       static_cast<long unsigned>(ninfile));
	    if (ninfile > (nold+nnew)*elem) {
		fclose(fdat);
		truncate(fn, (nold+nnew)*elem);
		fdat = fopen(fn, "ab");
	    }
	    else if (ninfile < nold*elem) {
		ninfile /= elem;
		for (uint32_t i = ninfile; i < nold; ++ i) {
		    // write NULLs
		    ierr = fwrite(&tmp, elem, 1, fdat);
		    if (ierr == 0)
			logError("writeData", "unable to write to \"%s\"",
				 fn);
		}
	    }
	    ierr = fseek(fdat, nold*elem, SEEK_SET);
	}
	if (ninfile > nold)
	    ninfile = nold;

	const signed char *arr = reinterpret_cast<const signed char*>(va1);
	signed char il, iu;
	il = arr[0];
	iu = arr[0];

	for (uint32_t i = 1; i < nnew; ++ i) {
	    if (arr[i] > iu) {
		iu = arr[i];
	    }
	    else if (arr[i] < il) {
		il = arr[i];
	    }
	}
	if (nold <= 0) {
	    lower = il;
	    upper = iu;
	}
	else {
	    if (lower > il) lower = il;
	    if (upper < iu) upper = iu;
	}

	nact = fwrite(arr, elem, nnew, fdat);
	fclose(fdat);
	if (nact < nnew ) {
	    logWarning("writeData", "expected to write %lu int "
		       "to \"%s\", but only wrote %lu",
		       static_cast<long unsigned>(nnew), fn,
		       static_cast<long unsigned>(nact));
	}
    }
    else if (m_type == ibis::FLOAT) {
	// data type is float -- single precision floating-point values
// #if INT_MAX == 0x7FFFFFFFL
// 	const int tmp = 0x7F800001; // NaN on a SUN workstation
// #else
// 	const int tmp = INT_MAX;	// likely also a NaN
// #endif
	const float tmp = std::numeric_limits<float>::quiet_NaN();
	const unsigned int elem = sizeof(float);
	if (ninfile != nold*elem) {
	    logMessage("writeData", "file \"%s\" is expected to have %lu "
		       "bytes but it has %lu", fn,
		       static_cast<long unsigned>(nold*elem),
		       static_cast<long unsigned>(ninfile));
	    if (ninfile < nold*elem) {
		ninfile /= elem;
		for (uint32_t i = ninfile; i < nold; ++ i) {
		    // write NULLs
		    ierr = fwrite(&tmp, elem, 1, fdat);
		    if (ierr == 0)
			logError("writeData", "unable to write to \"%s\"",
				 fn);
		}
	    }
	    else if (ninfile > (nold+nnew)*elem) {
		fclose(fdat);
		truncate(fn, (nold+nnew)*elem);
		fdat = fopen(fn, "ab");
	    }
	    ierr = fseek(fdat, nold*elem, SEEK_SET);
	}
	if (ninfile > nold)
	    ninfile = nold;

	const float *arr = reinterpret_cast<const float*>(va1);
	float il, iu;
	il = arr[0];
	iu = arr[0];

	for (uint32_t i = 1; i < nnew; ++ i) {
	    if (arr[i] > iu) {
		iu = arr[i];
	    }
	    else if (arr[i] < il) {
		il = arr[i];
	    }
	}
	if (nold <= 0) {
	    lower = il;
	    upper = iu;
	}
	else {
	    if (lower > il) lower = il;
	    if (upper < iu) upper = iu;
	}

	nact = fwrite(arr, elem, nnew, fdat);
	fclose(fdat);
	if (nact < nnew ) {
	    logWarning("writeData", "expected to write %lu float "
		       "to \"%s\", but only wrote %lu",
		       static_cast<long unsigned>(nnew), fn,
		       static_cast<long unsigned>(nact));
	}
    }
    else if (m_type == ibis::DOUBLE) {
	// data type is double -- double precision floating-point values
// #if INT_MAX == 0x7FFFFFFFL
// 	const int tmp[2]={0x7FFF0000, 0x00000001}; // NaN on a SUN workstation
// #else
// 	const int tmp[2]={INT_MAX, INT_MAX};
// #endif
	const double tmp = std::numeric_limits<double>::quiet_NaN();
	const unsigned int elem = sizeof(double);
	if (ninfile != nold*elem) {
	    logMessage("writeData", "file \"%s\" is expected to have %lu "
		       "bytes but it has %lu", fn,
		       static_cast<long unsigned>(nold*elem),
		       static_cast<long unsigned>(ninfile));
	    if (ninfile < nold*elem) {
		ninfile /= elem;
		for (uint32_t i = nact; i < nold; ++ i) {
		    // write NULLs
		    ierr = fwrite(&tmp, elem, 1, fdat);
		    if (ierr == 0)
			logError("writeData", "unable to write to \"%s\"",
				 fn);
		}
	    }
	    else if (ninfile > (nold+nnew)*elem) {
		fclose(fdat);
		truncate(fn, (nold+nnew)*elem);
		fdat = fopen(fn, "ab");
	    }
	    ierr = fseek(fdat, nold*elem, SEEK_SET);
	}
	if (ninfile > nold)
	    ninfile = nold;

	const double *arr = reinterpret_cast<const double*>(va1);

	for (uint32_t i = 0; i < nnew; ++ i) {
	    if (arr[i] > upper) {
		upper = arr[i];
	    }
	    if (arr[i] < lower) {
		lower = arr[i];
	    }
	}

	nact = fwrite(arr, elem, nnew, fdat);
	fclose(fdat);
	if (nact < nnew ) {
	    logWarning("writeData", "expected to write %lu double "
		       "to \"%s\", but only wrote %lu",
		       static_cast<long unsigned>(nnew), fn,
		       static_cast<long unsigned>(nact));
	}
    }
    else if (m_type == ibis::OID) {
	// OID is formed from two unsigned ints, i.e., both va1 and va2 are
	// used here
	// use logError to terminate this function upon any error
	if (va2 == 0 || va1 == 0) {
	    fclose(fdat);
	    logWarning("writeData", "both components of OID must be valid");
	    return 0;
	}
	else if (ninfile != 2*sizeof(int)*nold) {
	    fclose(fdat);
	    logWarning("writeData", "OID file \"%s\" is expected to %lu "
		       "bytes, but it has %lu", fn,
		       static_cast<long unsigned>(2*sizeof(int)*nold),
		       static_cast<long unsigned>(ninfile));
	    return 0;
	}
	else {
	    const unsigned int *rn = reinterpret_cast<const unsigned*>(va1);
	    const unsigned int *en = reinterpret_cast<const unsigned*>(va2);
	    for (nact = 0; nact < nnew; ++ nact) {
		ierr = fwrite(rn+nact, sizeof(unsigned), 1, fdat);
		ierr += fwrite(en+nact, sizeof(unsigned), 1, fdat);
		if (ierr != 2) {
		    fclose(fdat);
		    logWarning("writeData", "failed to write new OID # %lu "
			       "to \"%s\"", static_cast<long unsigned>(nact),
			       fn);
		    break;
		}
	    }
	    fclose(fdat);
	    if (nact != nnew) {
		logWarning("writeData", "nact(=%lu) must be the same as "
			   "nnew(=%lu) for the OID column, remove \"%s\"",
			   static_cast<long unsigned>(nact),
			   static_cast<long unsigned>(nnew), fn);
		(void) remove(fn);
		nact = 0;
	    }
	    return nact;
	}
    }
    else if (m_type == ibis::CATEGORY ||
	     m_type == ibis::TEXT) {
	// data type TEXT/CATEGORY -- string valued attributes to check the
	// size properly, we will have to go through the whole file.  To
	// avoid that expense, only do a minimum amount of checking
	uint32_t oldbytes = ninfile;
	if (nold > 0) { // check with mask file for ninfile
	    char tmp[1024];
	    (void) memset(tmp, 0, 1024);
	    ninfile = mask.size();
	    if (nold > ninfile) {
		if (ibis::gVerbose > 2)
		    logMessage("writeData", "adding %lu null string(s) "
			       "(mask.size()=%lu, nold=%lu)",
			       static_cast<long unsigned>(nold-ninfile),
			       static_cast<long unsigned>(ninfile),
			       static_cast<long unsigned>(nold));
		for (uint32_t i = ninfile; i < nold; i += 1024)
		    fwrite(tmp, 1, (nold-i>1024)?1024:(nold-i), fdat);
	    }
	}
	else {
	    ninfile = 0;
	}

	const char* arr = reinterpret_cast<const char*>(va1);
	const uint32_t nbytes =
	    *reinterpret_cast<const uint32_t*>(va2);
	nact = fwrite(arr, 1, nbytes, fdat);
	fclose(fdat);
	if (nact != nbytes) { // no easy way to recover
	    logWarning("writeData", "expected to write %lu bytes to "
		       "\"%s\", but only wrote %lu",
		       static_cast<long unsigned>(nbytes), fn,
		       static_cast<long unsigned>(nact));
	    truncate(fn, oldbytes);
	    nact = 0;
	}
	else {
	    if (ibis::gVerbose > 13)
		logMessage("writeData", "wrote %lu bytes as requested",
			   static_cast<long unsigned>(nact));
	    nact = nnew;
	}
    }
    else {
	fclose(fdat);
	logError("writeData", "data type %d not yet supported",
		 (int)(m_type));
	return 0;
    }

    if (ibis::gVerbose > 8) {
	ibis::util::logger lg;
	lg.buffer() << "column[" << (thePart != 0 ? thePart->name() : "")
		    << '.' << m_name << "](" << ibis::TYPESTRING[(int)m_type]
		    << ")::writeData -- wrote " << nact << " entr"
		    << (nact>1?"ies":"y") << " (expected " << nnew
		    << ") to " << fn << "\n";
	if (ibis::gVerbose > 16)
	    lg.buffer() << *this;
    }

    // part II: append new bits to update the null mask
    strcat(fn, ".msk");
    mask.adjustSize(ninfile, nold);
    mask.adjustSize(nact+nold, nnew+nold);
    if (mask.cnt() < mask.size()) {
	mask.write(fn);
	if (ibis::gVerbose > 7)
	    logMessage("writeData", "null mask in \"%s\" contains %lu set "
		       "bits and %lu total bits",
		       fn, static_cast<long unsigned>(mask.cnt()),
		       static_cast<long unsigned>(mask.size()));
    }
    else if (ibis::util::getFileSize(fn) > 0) {
	(void) remove(fn);
    }
    ibis::fileManager::instance().flushFile(fn);
    return nact;
} // ibis::column::writeData

/// Write the selected records to the specified directory.
/// Save only the rows marked 1.  Replace the data file in @c dest.
/// Return the number of rows written to the new file or a negative number
/// to indicate error.
long ibis::column::saveSelected(const ibis::bitvector& sel, const char *dest,
				char *buf, uint32_t nbuf) {
    const int elm = elementSize();
    if (thePart == 0 || thePart->currentDataDir() == 0 || elm <= 0)
	return -1;

    long ierr = 0;
    ibis::fileManager::buffer<char> mybuf(buf != 0);
    if (buf == 0) {
	nbuf = mybuf.size();
	buf = mybuf.address();
    }
    if (buf == 0) {
	throw new ibis::bad_alloc("saveSelected cannot allocate workspace");
    }

    if (dest == 0 || dest == thePart->currentDataDir() ||
	strcmp(dest, thePart->currentDataDir()) == 0) { // same directory
	std::string fname = thePart->currentDataDir();
	if (! fname.empty())
	    fname += FASTBIT_DIRSEP;
	fname += m_name;
	ibis::bitvector current;
	getNullMask(current);

	writeLock lock(this, "saveSelected");
	if (idx != 0) {
	    const uint32_t idxc = idxcnt();
	    if (0 == idxc) {
		delete idx;
		idx = 0;
		purgeIndexFile(thePart->currentDataDir());
	    }
	    else {
		logWarning("saveSelected", "index files are in-use, "
			   "should not overwrite data files");
		return -2;
	    }
	}
	ibis::fileManager::instance().flushFile(fname.c_str());
	FILE* fptr = fopen(fname.c_str(), "r+b");
	if (fptr == 0) {
	    if (ibis::gVerbose > -1)
		logWarning("saveSelected", "failed to open file \"%s\"",
			   fname.c_str());
	    return -3;
	}

	off_t pos = 0; // position to write the next byte
	for (ibis::bitvector::indexSet ix = sel.firstIndexSet();
	     ix.nIndices() > 0; ++ ix) {
	    const ibis::bitvector::word_t *idx = ix.indices();
	    if (ix.isRange()) {
		if ((size_t) pos < elm * *idx) {
		    const off_t endpos = idx[1] * elm;
		    for (off_t j = *idx * elm; j < endpos; j += nbuf) {
			fflush(fptr); // prepare for reading
			ierr = fseek(fptr, j, SEEK_SET);
			if (ierr != 0) {
			    if (ibis::gVerbose > 0)
				logWarning("saveSelected", "failed to seek to "
					   "%lu in file \"%s\"",
					   static_cast<long unsigned>(j),
					   fname.c_str());
			    ierr = -4;
			    fclose(fptr);
			    return ierr;
			}

			off_t nbytes = (j+(off_t)nbuf <= endpos ?
					nbuf : endpos-j);
			ierr = fread(buf, 1, nbytes, fptr);
			if (ierr < 0) {
			    if (ibis::gVerbose > 0)
				logWarning("saveSelected", "failed to read "
					   "file \"%s\" at position %lu, "
					   "fill buffer with 0",
					   fname.c_str(),
					   static_cast<long unsigned>(j));
			    ierr = 0;
			}
			for (; ierr < nbytes; ++ ierr)
			    buf[ierr] = 0;

			fflush(fptr); // prepare to write
			ierr = fseek(fptr, pos, SEEK_SET);
			ierr += fwrite(buf, 1, nbytes, fptr);
			if (ierr < nbytes) {
			    if (ibis::gVerbose > 0)
				logWarning("saveSelected", "failed to write "
					   "%lu bytes to file \"%s\" at "
					   "position %lu",
					   static_cast<long unsigned>(nbytes),
					   fname.c_str(),
					   static_cast<long unsigned>(pos));
			}
			pos += nbytes;
		    } // for (off_t j...
		}
		else { // don't need to write anything here
		    pos += elm * (idx[1] - *idx);
		}
	    }
	    else {
		fflush(fptr);
		ierr = fseek(fptr, *idx * elm, SEEK_SET);
		if (ierr != 0) {
		    if (ibis::gVerbose > 0)
			logWarning("saveSelected", "failed to seek to "
				   "%lu in file \"%s\"",
				   static_cast<long unsigned>(*idx * elm),
				   fname.c_str());
		    ierr = -5;
		    fclose(fptr);
		    return ierr;
		}
		const off_t nread = elm * (idx[ix.nIndices()-1] - *idx + 1);
		ierr = fread(buf, 1, nread, fptr);
		if (ierr < 0) {
		    if (ibis::gVerbose > 0)
			logWarning("saveSelected", "failed to read "
				   "file \"%s\" at position %lu, "
				   "fill buffer with 0",
				   fname.c_str(), ierr);
		    ierr = 0;
		}
		for (; ierr < nread; ++ ierr)
		    buf[ierr] = static_cast<char>(0);

		fflush(fptr); // prepare to write
		ierr = fseek(fptr, pos, SEEK_SET);
		for (uint32_t j = 0; j < ix.nIndices(); ++ j) {
		    ierr = fwrite(buf + elm * (idx[j] - *idx), 1, elm, fptr);
		    if (ierr < elm) {
			if (ibis::gVerbose > 0)
			    logWarning("saveSelected", "failed to write a "
				       "%d-byte element to %lu in file \"%s\"",
				       elm, static_cast<long unsigned>(pos),
				       fname.c_str());
		    }
		    pos += elm;
		}
	    }
	}
	fclose(fptr);
	(void) truncate(fname.c_str(), pos);
	ierr = static_cast<long>(pos / elm);
	if (ibis::gVerbose > 1)
	    logMessage("saveSelected", "rewrote data file %s with %ld row%s",
		       fname.c_str(), ierr, (ierr > 1 ? "s" : ""));

	ibis::bitvector bv;
	current.subset(sel, bv);
	fname += ".msk";

	mutexLock mtx(this, "saveSelected");
	mask_.swap(bv);
	if (mask_.size() > mask_.cnt())
	    mask_.write(fname.c_str());
	else
	    remove(fname.c_str());
	if (ibis::gVerbose > 3)
	    logMessage("saveSelected", "new column mask %lu out of %lu",
		       static_cast<long unsigned>(mask_.cnt()),
		       static_cast<long unsigned>(mask_.size()));
    }
    else { // different directory
	std::string sfname = thePart->currentDataDir();
	std::string dfname = dest;
	if (! sfname.empty()) sfname += FASTBIT_DIRSEP;
	if (! dfname.empty()) dfname += FASTBIT_DIRSEP;
	sfname += m_name;
	dfname += m_name;

	purgeIndexFile(dest);
	readLock lock(this, "saveSelected");
	FILE* sfptr = fopen(sfname.c_str(), "rb");
	if (sfptr == 0) {
	    if (ibis::gVerbose > 0)
		logWarning("saveSelected", "failed to open file \"%s\" for "
			   "reading", sfname.c_str());
	    return -6;
	}
	ibis::fileManager::instance().flushFile(dfname.c_str());
	FILE* dfptr = fopen(dfname.c_str(), "wb");
	if (dfptr == 0) {
	    if (ibis::gVerbose > 0)
		logWarning("saveSelected", "failed to open file \"%s\" for "
			   "writing", dfname.c_str());
	    fclose(sfptr);
	    return -7;
	}

	for (ibis::bitvector::indexSet ix = sel.firstIndexSet();
	     ix.nIndices() > 0; ++ ix) {
	    const ibis::bitvector::word_t *idx = ix.indices();
	    ierr = fseek(sfptr, *idx * elm, SEEK_SET);
	    if (ierr != 0) {
		if (ibis::gVerbose > 0)
		    logWarning("saveSelected", "failed to seek to %ld in "
			       "file \"%s\"", static_cast<long>(*idx * elm),
			       sfname.c_str());
		fclose(sfptr);
		fclose(dfptr);
		return -8;
	    }

	    if (ix.isRange()) {
		const off_t endblock = idx[1] * elm;
		for (off_t j = *idx * elm; j < endblock; j += nbuf) {
		    const off_t nbytes =
			elm * (j+(off_t)nbuf <= endblock ? nbuf : endblock-j);
		    ierr = fread(buf, 1, nbytes, sfptr);
		    if (ierr < 0) {
			if (ibis::gVerbose > 0)
			    logWarning("saveSelected", "failed to read from "
				       "\"%s\" at position %lu, fill buffer "
				       "with 0", sfname.c_str(),
				       static_cast<long unsigned>(j));
			ierr = 0;
		    }
		    for (; ierr < nbytes; ++ ierr)
			buf[ierr] = static_cast<char>(0);
		    ierr = fwrite(buf, 1, nbytes, dfptr);
		    if (ierr < nbytes && ibis::gVerbose > 0)
			logWarning("saveSelected", "expected to write %lu "
				   "bytes to \"%s\", but only wrote %ld",
				   static_cast<long unsigned>(nbytes),
				   dfname.c_str(), static_cast<long>(ierr));
		}
	    }
	    else {
		const off_t nbytes = elm * (idx[ix.nIndices()-1] - *idx + 1);
		ierr = fread(buf, 1, nbytes, sfptr);
		if (ierr < 0) {
		    if (ibis::gVerbose > 0)
			logWarning("saveSelected", "failed to read from "
				   "\"%s\" at position %lu, fill buffer "
				   "with 0", sfname.c_str(),
				   static_cast<long unsigned>(*idx * elm));
		    ierr = 0;
		}
		for (; ierr < nbytes; ++ ierr)
		    buf[ierr] = static_cast<char>(0);
		for (uint32_t j = 0; j < ix.nIndices(); ++ j) {
		    ierr = fwrite(buf + elm * (idx[j] - *idx), 1, elm, dfptr);
		    if (ierr < elm && ibis::gVerbose > 0)
			logWarning("saveSelected", "expected to write a "
				   "%d-byte element to \"%s\", but only "
				   "wrote %d byte(s)", elm,
				   dfname.c_str(), static_cast<int>(ierr));
		}
	    }
	}
	if (ibis::gVerbose > 1)
	    logMessage("saveSelected", "copied %ld row%s from %s to %s",
		       ierr, (ierr > 1 ? "s" : ""), sfname.c_str(),
		       dfname.c_str());

	ibis::bitvector current, bv;
	getNullMask(current);
	current.subset(sel, bv);
	dfname += ".msk";
	if (bv.size() != bv.cnt())
	    bv.write(dfname.c_str());
	else
	    remove(dfname.c_str());
	if (ibis::gVerbose > 3)
	    logMessage("saveSelected", "saved new mask (%lu out of %lu) to %s",
		       static_cast<long unsigned>(bv.cnt()),
		       static_cast<long unsigned>(bv.size()),
		       dfname.c_str());
    }

    return ierr;
} // ibis::column::saveSelected

/// Truncate the number of records in the named dir to nent.  It truncates
/// file if more entries are in the current file, and it adds more NULL
/// values if the current file is shorter.  The null mask is adjusted
/// accordingly.
long ibis::column::truncateData(const char* dir, uint32_t nent,
				ibis::bitvector& mask) const {
    long ierr = 0;
    if (dir == 0)
	return -1;
    char fn[MAX_LINE];
#if defined(sun) && defined(__GNUC__) && __GNUC__ <= 2
    ierr = sprintf(fn, "%s%c%s", dir, FASTBIT_DIRSEP, m_name.c_str());
#else
    ierr = UnixSnprintf(fn, MAX_LINE, "%s%c%s", dir, FASTBIT_DIRSEP, m_name.c_str());
#endif
    if (ierr <= 0 || ierr > MAX_LINE) {
	logWarning("truncateData", "failed to generate data file name, "
		   "name (%s%c%s) too long", dir, FASTBIT_DIRSEP, m_name.c_str());
	return -2;
    }

    uint32_t nact = 0; // number of valid entries left in the file
    uint32_t nbyt = 0; // number of bytes in the file to be left
    char buf[MAX_LINE];
    if (m_type == ibis::CATEGORY ||
	m_type == ibis::TEXT) {
	// character strings -- need to read the content file
	array_t<char> *arr = new array_t<char>;
	ierr = ibis::fileManager::instance().getFile(fn, *arr);
	if (ierr == 0) {
	    uint32_t cnt = 0;
	    const char *end = arr->end();
	    const char *ptr = arr->begin();
	    while (cnt < nent && ptr < end) {
		cnt += (*ptr == 0);
		++ ptr;
	    }
	    nact = cnt;
	    nbyt = ptr - arr->begin();
	    delete arr; // no longer need the array_t
	    ibis::fileManager::instance().flushFile(fn);

	    if (cnt < nent) { // current file does not have enough entries
		memset(buf, 0, MAX_LINE);
		FILE *fptr = fopen(fn, "ab");
		while (cnt < nent) {
		    uint32_t nb = nent - cnt;
		    if (nb > MAX_LINE)
			nb = MAX_LINE;
		    ierr = fwrite(buf, 1, nb, fptr);
		    if (static_cast<uint32_t>(ierr) != nb) {
			logWarning("truncateData", "expected to write "
				   "%lu bytes to \"%s\", but only wrote "
				   "%ld", static_cast<long unsigned>(nb),
				   fn, ierr);
			if (ierr == 0) {
			    ierr = -1;
			    break;
			}
		    }
		    cnt += ierr;
		}
		nbyt = ftell(fptr);
		fclose(fptr);
	    }
	    ierr = (ierr>=0 ? 0 : -1);
	}
	else {
	    logWarning("truncateData", "failed to open \"%s\" using the "
		       "file manager, ierr=%ld", fn, ierr);
	    FILE *fptr = fopen(fn, "rb+"); // open for read and write
	    if (fptr != 0) {
                uint32_t cnt = 0;
                while (cnt < nent) {
		    ierr = fread(buf, 1, MAX_LINE, fptr);
		    if (ierr == 0) break;
		    int i = 0;
		    for (i = 0; cnt < nent && i < MAX_LINE; ++ i)
			cnt += (buf[i] == 0);
		    nbyt += i;
		}
		nact = cnt;

		if (cnt < nent) { // need to write more null characters
		    memset(buf, 0, MAX_LINE);
		    while (cnt < nent) {
			uint32_t nb = nent - cnt;
			if (nb > MAX_LINE)
			    nb = MAX_LINE;
			ierr = fwrite(buf, 1, nb, fptr);
			if (static_cast<uint32_t>(ierr) != nb) {
			    logWarning("truncateData", "expected to write "
				       "%lu bytes to \"%s\", but only wrote "
				       "%ld", static_cast<long unsigned>(nb),
				       fn, ierr);
			    if (ierr == 0) {
				ierr = -1;
				break;
			    }
			}
			cnt += ierr;
		    }
		    nbyt = ftell(fptr);
		}
		fclose(fptr);
		ierr = (ierr >= 0 ? 0 : -1);
	    }
	    else {
		logWarning("truncateData", "failed to open \"%s\" with "
			   "fopen, file probably does not exist or has "
			   "wrong perssions", fn);
		ierr = -1;
	    }
	}
    }
    else { // other fixed size columns
	const uint32_t elm = elementSize();
	nbyt = ibis::util::getFileSize(fn);
	nact = nbyt / elm;
	if (nact < nent) { // needs to write more entries to the file
	    FILE *fptr = fopen(fn, "ab");
	    if (fptr != 0) {
		uint32_t cnt = nact;
		memset(buf, 0, MAX_LINE);
		while (cnt < nent) {
		    uint32_t nb = (nent - cnt) * elm;
		    if (nb > MAX_LINE)
			nb = ((MAX_LINE / elm) * elm);
		    ierr = fwrite(buf, 1, nb, fptr);
		    if (static_cast<uint32_t>(ierr) != nb) {
			logWarning("truncateData", "expected to write "
				   "%lu bytes to \"%s\", but only wrote "
				   "%ld", static_cast<long unsigned>(nb),
				   fn, ierr);
			if (ierr == 0) {
			    ierr = -1;
			    break;
			}
		    }
		    cnt += ierr;
		}
		nbyt = ftell(fptr);
		fclose(fptr);
		ierr = (ierr >= 0 ? 0 : -1);
	    }
	    else {
		logWarning("truncateData", "failed to open \"%s\" with "
			   "fopen, make sure the directory exist and has "
			   "right perssions", fn);
		ierr = -1;
	    }
	}
    }

    // actually tuncate the file here
    if (ierr == 0) {
	ierr = truncate(fn, nbyt);
	if (ierr != 0) {
	    logWarning("truncateData", "failed to truncate \"%s\" to "
		       "%lu bytes, ierr=%ld", fn,
		       static_cast<long unsigned>(nbyt), ierr);
	    ierr = -2;
	}
	else {
	    ierr = nent;
	    if (ibis::gVerbose > 8)
		logMessage("truncateData", "successfully trnncated \"%s\" "
			   "to %lu bytes (%lu records)", fn,
			   static_cast<long unsigned>(nbyt),
			   static_cast<long unsigned>(nent));
	}
    }

    // dealing with the null mask
    strcat(fn, ".msk");
    mask.adjustSize(nact, nent);
    if (mask.cnt() < mask.size()) {
	mask.write(fn);
	if (ibis::gVerbose > 7)
	    logMessage("truncateData", "null mask in \"%s\" contains %lu "
		       "set bits and %lu total bits", fn,
		       static_cast<long unsigned>(mask.cnt()),
		       static_cast<long unsigned>(mask.size()));
    }
    else if (ibis::util::getFileSize(fn) > 0) {
	(void) remove(fn);
    }
    return ierr;
} // ibis::column::truncateData

/// Cast the incoming array into the specified type T before writing the
/// values to the file for this column.  This function uses assignment
/// statements to perform the casting operations.  Warning: this function
/// does not check that the cast values are equal to the incoming values!
template <typename T>
long ibis::column::castAndWrite(const array_t<double>& vals,
				ibis::bitvector& mask, const T special) {
    array_t<T> tmp(mask.size());
    ibis::bitvector::word_t jtmp = 0;
    ibis::bitvector::word_t jvals = 0;
    for (ibis::bitvector::indexSet is = mask.firstIndexSet();
	 is.nIndices() > 0; ++ is) {
	const ibis::bitvector::word_t *idx = is.indices();
	while (jtmp < *idx) {
	    tmp[jtmp] = special;
	    ++ jtmp;
	}
	if (is.isRange()) {
	    while (jtmp < idx[1]) {
		if (lower > vals[jvals])
		    lower = vals[jvals];
		if (upper < vals[jvals])
		    upper = vals[jvals];
		tmp[jtmp] = vals[jvals];
		++ jvals;
		++ jtmp;
	    }
	}
	else {
	    for (unsigned i = 0; i < is.nIndices(); ++ i) {
		while (jtmp < idx[i]) {
		    tmp[jtmp] = special;
		    ++ jtmp;
		}
		if (lower > vals[jvals])
		    lower = vals[jvals];
		if (upper < vals[jvals])
		    upper = vals[jvals];
		tmp[jtmp] = vals[jvals];
		++ jvals;
		++ jtmp;
	    }
	}
    }
    while (jtmp < mask.size()) {
	tmp[jtmp] = special;
	++ jtmp;
    }
    long ierr = writeData(thePart->currentDataDir(), 0, mask.size(), mask,
			  tmp.begin(), 0);
    return ierr;
} // ibis::column::castAndWrite
			      
template <typename T>
void ibis::column::actualMinMax(const array_t<T>& vals,
				const ibis::bitvector& mask,
				double& min, double& max) const {
    min = DBL_MAX;
    max = - DBL_MAX;
    if (vals.empty() || mask.cnt() == 0) return;

    T amin = std::numeric_limits<T>::max();
    T amax = std::numeric_limits<T>::min();
    for (ibis::bitvector::indexSet ix = mask.firstIndexSet();
	 ix.nIndices() > 0; ++ ix) {
	const ibis::bitvector::word_t *idx = ix.indices();
	if (ix.isRange()) {
	    ibis::bitvector::word_t last = (idx[1] <= vals.size() ?
					    idx[1] : vals.size());
	    for (uint32_t i = *idx; i < last; ++ i) {
		amin = (amin > vals[i] ? vals[i] : amin);
		amax = (amax < vals[i] ? vals[i] : amax);
	    }
	}
	else {
	    for (uint32_t i = 0; i < ix.nIndices() && idx[i] < vals.size();
		 ++ i) {
		amin = (amin > vals[idx[i]] ? vals[idx[i]] : amin);
		amax = (amax < vals[idx[i]] ? vals[idx[i]] : amax);
	    }
	}
    }

    min = static_cast<double>(amin);
    max = static_cast<double>(amax);
    LOGGER(ibis::gVerbose > 5)
	<< "column[" << (thePart!=0 ? thePart->name() : "") << "."
	<< m_name << "]::actualMinMax -- vals.size() = "
	<< vals.size() << ", mask.cnt() = " << mask.cnt()
	<< ", min = " << min << ", max = " << max;
} // ibis::column::actualMinMax

template <typename T>
T ibis::column::computeMin(const array_t<T>& vals,
			   const ibis::bitvector& mask) const {
    T res = std::numeric_limits<T>::max();
    if (vals.empty() || mask.cnt() == 0) return res;

    for (ibis::bitvector::indexSet ix = mask.firstIndexSet();
	 ix.nIndices() > 0; ++ ix) {
	const ibis::bitvector::word_t *idx = ix.indices();
	if (ix.isRange()) {
	    ibis::bitvector::word_t last = (idx[1] <= vals.size() ?
					    idx[1] : vals.size());
	    for (uint32_t i = *idx; i < last; ++ i) {
		res = (res > vals[i] ? vals[i] : res);
	    }
	}
	else {
	    for (uint32_t i = 0; i < ix.nIndices() && idx[i] < vals.size();
		 ++ i) {
		res = (res > vals[idx[i]] ? vals[idx[i]] : res);
	    }
	}
    }

    if (ibis::gVerbose > 5) {
	ibis::util::logger lg;
	lg.buffer() << "column[" << (thePart!=0 ? thePart->name() : "") << "."
		    << m_name << "]::computeMin -- vals.size() = "
		    << vals.size() << ", mask.cnt() = " << mask.cnt()
		    << ", min = ";
	if (strstr(typeid(T).name(), "char") != 0)
	    lg.buffer() << (int)res;
	else
	    lg.buffer() << res;
    }
    return res;
} // ibis::column::computeMin

template <typename T>
T ibis::column::computeMax(const array_t<T>& vals,
			   const ibis::bitvector& mask) const {
    T res = std::numeric_limits<T>::min();
    if (vals.empty() || mask.cnt() == 0) return res;

    for (ibis::bitvector::indexSet ix = mask.firstIndexSet();
	 ix.nIndices() > 0; ++ ix) {
	const ibis::bitvector::word_t *idx = ix.indices();
	if (ix.isRange()) {
	    ibis::bitvector::word_t last = (idx[1] <= vals.size() ?
					    idx[1] : vals.size());
	    for (uint32_t i = *idx; i < last; ++ i) {
		res = (res < vals[i] ? vals[i] : res);
	    }
	}
	else {
	    for (uint32_t i = 0; i < ix.nIndices() && idx[i] < vals.size();
		 ++ i) {
		res = (res < vals[idx[i]] ? vals[idx[i]] : res);
	    }
	}
    }

    if (ibis::gVerbose > 5) {
	ibis::util::logger lg;
	lg.buffer() << "column[" << (thePart!=0 ? thePart->name() : "") << "."
		    << m_name << "]::computeMax -- vals.size() = "
		    << vals.size() << ", mask.cnt() = " << mask.cnt()
		    << ", max = ";
	if (strstr(typeid(T).name(), "char") != 0)
	    lg.buffer() << (int)res << std::endl;
	else
	    lg.buffer() << res << std::endl;
    }
    return res;
} // ibis::column::computeMax

template <typename T>
double ibis::column::computeSum(const array_t<T>& vals,
				const ibis::bitvector& mask) const {
    double res = 0.0;
    if (vals.empty() || mask.cnt() == 0) return res;

    for (ibis::bitvector::indexSet ix = mask.firstIndexSet();
	 ix.nIndices() > 0; ++ ix) {
	const ibis::bitvector::word_t *idx = ix.indices();
	if (ix.isRange()) {
	    ibis::bitvector::word_t last = (idx[1] <= vals.size() ?
					    idx[1] : vals.size());
	    for (uint32_t i = *idx; i < last; ++ i) {
		res += vals[i];
	    }
	}
	else {
	    for (uint32_t i = 0; i < ix.nIndices() && idx[i] < vals.size();
		 ++ i) {
		res += vals[idx[i]];
	    }
	}
    }

    LOGGER(ibis::gVerbose > 5)
	<< "column[" << (thePart!=0 ? thePart->name() : "") << "." << m_name
	<< "]::computeSum -- vals.size() = "
	<< vals.size() << ", mask.cnt() = " << mask.cnt() << ", sum = " << res;
    return res;
} // ibis::column::computeSum

// actually go through values and determine the min/max values, the min is
// recordeed as lowerBound and the max is recorded as the upperBound
double ibis::column::computeMin() const {
    double ret = DBL_MAX;
    if (thePart->nRows() == 0) return ret;

    ibis::bitvector mask;
    getNullMask(mask);
    if (mask.cnt() == 0) return ret;

    std::string sname;
    const char* name = dataFileName(sname);
    if (name == 0) return ret;

    switch (m_type) {
    case ibis::UBYTE: {
	array_t<unsigned char> val;
	int ierr = ibis::fileManager::instance().getFile(name, val);
	if (ierr != 0) {
	    logWarning("computeMin", "unable to retrieve file %s", name);
	}
	else {
	    ret = computeMin(val, mask);
	}
	break;}
    case ibis::BYTE: {
	array_t<signed char> val;
	int ierr = ibis::fileManager::instance().getFile(name, val);
	if (ierr != 0) {
	    logWarning("computeMin", "unable to retrieve file %s", name);
	}
	else {
	    ret = computeMin(val, mask);
	}
	break;}
    case ibis::USHORT: {
	array_t<uint16_t> val;
	int ierr = ibis::fileManager::instance().getFile(name, val);
	if (ierr != 0) {
	    logWarning("computeMin", "unable to retrieve file %s", name);
	}
	else {
	    ret = computeMin(val, mask);
	}
	break;}
    case ibis::SHORT: {
	array_t<int16_t> val;
	int ierr = ibis::fileManager::instance().getFile(name, val);
	if (ierr != 0) {
	    logWarning("computeMin", "unable to retrieve file %s", name);
	}
	else {
	    ret = computeMin(val, mask);
	}
	break;}
    case ibis::UINT: {
	array_t<uint32_t> val;
	int ierr = ibis::fileManager::instance().getFile(name, val);
	if (ierr != 0) {
	    logWarning("computeMin", "unable to retrieve file %s", name);
	}
	else {
	    ret = computeMin(val, mask);
	}
	break;}
    case ibis::INT: {
	array_t<int32_t> val;
	int ierr = ibis::fileManager::instance().getFile(name, val);
	if (ierr != 0) {
	    logWarning("computeMin", "unable to retrieve file %s", name);
	}
	else {
	    ret = computeMin(val, mask);
	}
	break;}
    case ibis::ULONG: {
	array_t<uint64_t> val;
	int ierr = ibis::fileManager::instance().getFile(name, val);
	if (ierr != 0) {
	    logWarning("computeMin", "unable to retrieve file %s", name);
	}
	else {
	    ret = computeMin(val, mask);
	}
	break;}
    case ibis::LONG: {
	array_t<int64_t> val;
	int ierr = ibis::fileManager::instance().getFile(name, val);
	if (ierr != 0) {
	    logWarning("computeMin", "unable to retrieve file %s", name);
	}
	else {
	    ret = computeMin(val, mask);
	}
	break;}
    case ibis::FLOAT: {
	array_t<float> val;
	int ierr = ibis::fileManager::instance().getFile(name, val);
	if (ierr != 0) {
	    logWarning("computeMin", "unable to retrieve file %s", name);
	}
	else {
	    ret = computeMin(val, mask);
	}
	break;}
    case ibis::DOUBLE: {
	array_t<double> val;
	int ierr = ibis::fileManager::instance().getFile(name, val);
	if (ierr != 0) {
	    logWarning("computeMin", "unable to retrieve file %s", name);
	}
	else {
	    ret = computeMin(val, mask);
	}
	break;}
    default:
	logMessage("computeMin", "not able to compute min");
    } // switch(m_type)

    return ret;
} // ibis::column::computeMin

// actually go through values and determine the min/max values, the min is
// recordeed as lowerBound and the max is recorded as the upperBound
double ibis::column::computeMax() const {
    double res = -DBL_MAX;
    if (thePart->nRows() == 0) return res;

    ibis::bitvector mask;
    getNullMask(mask);
    if (mask.cnt() == 0) return res;

    std::string sname;
    const char* name = dataFileName(sname);
    if (name == 0) return res;

    switch (m_type) {
    case ibis::UBYTE: {
	array_t<unsigned char> val;
	int ierr = ibis::fileManager::instance().getFile(name, val);
	if (ierr != 0) {
	    logWarning("computeMax", "unable to retrieve file %s", name);
	}
	else {
	    res = computeMax(val, mask);
	}
	break;}
    case ibis::BYTE: {
	array_t<signed char> val;
	int ierr = ibis::fileManager::instance().getFile(name, val);
	if (ierr != 0) {
	    logWarning("computeMax", "unable to retrieve file %s", name);
	}
	else {
	    res = computeMax(val, mask);
	}
	break;}
    case ibis::USHORT: {
	array_t<uint16_t> val;
	int ierr = ibis::fileManager::instance().getFile(name, val);
	if (ierr != 0) {
	    logWarning("computeMax", "unable to retrieve file %s", name);
	}
	else {
	    res = computeMax(val, mask);
	}
	break;}
    case ibis::SHORT: {
	array_t<int16_t> val;
	int ierr = ibis::fileManager::instance().getFile(name, val);
	if (ierr != 0) {
	    logWarning("computeMax", "unable to retrieve file %s", name);
	}
	else {
	    res = computeMax(val, mask);
	}
	break;}
    case ibis::UINT: {
	array_t<uint32_t> val;
	int ierr = ibis::fileManager::instance().getFile(name, val);
	if (ierr != 0) {
	    logWarning("computeMax", "unable to retrieve file %s", name);
	}
	else {
	    res = computeMax(val, mask);
	}
	break;}
    case ibis::INT: {
	array_t<int32_t> val;
	int ierr = ibis::fileManager::instance().getFile(name, val);
	if (ierr != 0) {
	    logWarning("computeMax", "unable to retrieve file %s", name);
	}
	else {
	    res = computeMax(val, mask);
	}
	break;}
    case ibis::ULONG: {
	array_t<uint64_t> val;
	int ierr = ibis::fileManager::instance().getFile(name, val);
	if (ierr != 0) {
	    logWarning("computeMax", "unable to retrieve file %s", name);
	}
	else {
	    res = computeMax(val, mask);
	}
	break;}
    case ibis::LONG: {
	array_t<int64_t> val;
	int ierr = ibis::fileManager::instance().getFile(name, val);
	if (ierr != 0) {
	    logWarning("computeMax", "unable to retrieve file %s", name);
	}
	else {
	    res = computeMax(val, mask);
	}
	break;}
    case ibis::FLOAT: {
	array_t<float> val;
	int ierr = ibis::fileManager::instance().getFile(name, val);
	if (ierr != 0) {
	    logWarning("computeMax", "unable to retrieve file %s", name);
	}
	else {
	    res = computeMax(val, mask);
	}
	break;}
    case ibis::DOUBLE: {
	array_t<double> val;
	int ierr = ibis::fileManager::instance().getFile(name, val);
	if (ierr != 0) {
	    logWarning("computeMax", "unable to retrieve file %s", name);
	}
	else {
	    res = computeMax(val, mask);
	}
	break;}
    default:
	logMessage("computeMax", "not able to compute max");
    } // switch(m_type)

    return res;
} // ibis::column::computeMax

double ibis::column::computeSum() const {
    double ret = 0;
    if (thePart->nRows() == 0) return ret;

    ibis::bitvector mask;
    getNullMask(mask);
    if (mask.cnt() == 0) return ret;

    std::string sname;
    const char* name = dataFileName(sname);
    if (name == 0) return ret;

    switch (m_type) {
    case ibis::UBYTE: {
	array_t<unsigned char> val;
	int ierr = ibis::fileManager::instance().getFile(name, val);
	if (ierr != 0) {
	    logWarning("computeSum", "unable to retrieve file %s", name);
	    ibis::util::setNaN(ret);
	}
	else {
	    ret = computeSum(val, mask);
	}
	break;}
    case ibis::BYTE: {
	array_t<signed char> val;
	int ierr = ibis::fileManager::instance().getFile(name, val);
	if (ierr != 0) {
	    logWarning("computeSum", "unable to retrieve file %s", name);
	    ibis::util::setNaN(ret);
	}
	else {
	    ret = computeSum(val, mask);
	}
	break;}
    case ibis::USHORT: {
	array_t<uint16_t> val;
	int ierr = ibis::fileManager::instance().getFile(name, val);
	if (ierr != 0) {
	    logWarning("computeSum", "unable to retrieve file %s", name);
	    ibis::util::setNaN(ret);
	}
	else {
	    ret = computeSum(val, mask);
	}
	break;}
    case ibis::SHORT: {
	array_t<int16_t> val;
	int ierr = ibis::fileManager::instance().getFile(name, val);
	if (ierr != 0) {
	    logWarning("computeSum", "unable to retrieve file %s", name);
	    ibis::util::setNaN(ret);
	}
	else {
	    ret = computeSum(val, mask);
	}
	break;}
    case ibis::UINT: {
	array_t<uint32_t> val;
	int ierr = ibis::fileManager::instance().getFile(name, val);
	if (ierr != 0) {
	    logWarning("computeSum", "unable to retrieve file %s", name);
	    ibis::util::setNaN(ret);
	}
	else {
	    ret = computeSum(val, mask);
	}
	break;}
    case ibis::INT: {
	array_t<int32_t> val;
	int ierr = ibis::fileManager::instance().getFile(name, val);
	if (ierr != 0) {
	    logWarning("computeSum", "unable to retrieve file %s", name);
	    ibis::util::setNaN(ret);
	}
	else {
	    ret = computeSum(val, mask);
	}
	break;}
    case ibis::ULONG: {
	array_t<uint64_t> val;
	int ierr = ibis::fileManager::instance().getFile(name, val);
	if (ierr != 0) {
	    logWarning("computeSum", "unable to retrieve file %s", name);
	    ibis::util::setNaN(ret);
	}
	else {
	    ret = computeSum(val, mask);
	}
	break;}
    case ibis::LONG: {
	array_t<int64_t> val;
	int ierr = ibis::fileManager::instance().getFile(name, val);
	if (ierr != 0) {
	    logWarning("computeSum", "unable to retrieve file %s", name);
	    ibis::util::setNaN(ret);
	}
	else {
	    ret = computeSum(val, mask);
	}
	break;}
    case ibis::FLOAT: {
	array_t<float> val;
	int ierr = ibis::fileManager::instance().getFile(name, val);
	if (ierr != 0) {
	    logWarning("computeSum", "unable to retrieve file %s", name);
	    ibis::util::setNaN(ret);
	}
	else {
	    ret = computeSum(val, mask);
	}
	break;}
    case ibis::DOUBLE: {
	array_t<double> val;
	int ierr = ibis::fileManager::instance().getFile(name, val);
	if (ierr != 0) {
	    logWarning("computeSum", "unable to retrieve file %s", name);
	    ibis::util::setNaN(ret);
	}
	else {
	    ret = computeSum(val, mask);
	}
	break;}
    default:
	logMessage("computeSum", "not able to compute sum");
    } // switch(m_type)

    return ret;
} // ibis::column::computeSum

double ibis::column::getActualMin() const {
    double ret;
    indexLock lock(this, "getActualMin");
    if (idx != 0) {
	ret = idx->getMin();
	if (! (ret < 0.0 || ret >= 0.0))
	    ret = computeMin();
    }
    else {
	ret = computeMin();
    }
    return ret;
} // ibis::column::getActualMin

double ibis::column::getActualMax() const {
    double ret;
    indexLock lock(this, "getActualMax");
    if (idx != 0) {
	ret = idx->getMax();
	if (! (ret < 0.0 || ret >= 0.0))
	    ret = computeMax();
    }
    else {
	ret = computeMax();
    }
    return ret;
} // ibis::column::getActualMax

double ibis::column::getSum() const {
    double ret;
    indexLock lock(this, "getSum");
    if (idx != 0) {
	ret = idx->getSum();
	if (! (ret < 0.0 || ret >= 0.0))
	    ret = computeSum();
    }
    else {
	ret = computeSum();
    }
    return ret;
} // ibis::column::getSum

long ibis::column::getCumulativeDistribution
(std::vector<double>& bds, std::vector<uint32_t>& cts) const {
    indexLock lock(this, "getCumulativeDistribution");
    long ierr = -1;
    if (idx != 0) {
	ierr = idx->getCumulativeDistribution(bds, cts);
	if (ierr < 0)
	    ierr += -10;
    }
    return ierr;
} // ibis::column::getCumulativeDistribution

long ibis::column::getDistribution
(std::vector<double>& bds, std::vector<uint32_t>& cts) const {
    indexLock lock(this, "getDistribution");
    long ierr = -1;
    if (idx != 0) {
	ierr = idx->getDistribution(bds, cts);
	if (ierr < 0)
	    ierr += -10;
    }
    return ierr;
} // ibis::column::getDistribution

/// Change the m_sorted flag.  If the flag m_sorted is set to true, the
/// caller should have sorted the data file.  Incorrect flag will lead to
/// wrong answers to queries.  This operation invokes a write lock on the
/// column object.
void ibis::column::isSorted(bool iss) {
    writeLock lock(this, "isSorted");
    m_sorted = iss;
} // ibis::column::isSorted

int ibis::column::searchSorted(const ibis::qContinuousRange& rng,
			       ibis::bitvector& hits) const {
    std::string dfname;
    if (dataFileName(dfname) == 0) {
	LOGGER(ibis::gVerbose >= 0)
	    << "Warning -- column[" << (thePart ? thePart->name() : "?") << '.'
	    << m_name << "]::searchSorted(" << rng
	    << ") failed to determine the data file name";
	return -4;
    }

    int ierr;
    switch (m_type) {
    case ibis::BYTE: {
	array_t<char> vals;
	ierr = ibis::fileManager::instance().getFile(dfname.c_str(), vals);
	if (ierr == 0) {
	    ierr = searchSortedICC(vals, rng, hits);
	}
	else {
	    ierr = searchSortedOOCC<char>(dfname.c_str(), rng, hits);
	}
	break;}
    case ibis::UBYTE: {
	array_t<unsigned char> vals;
	ierr = ibis::fileManager::instance().getFile(dfname.c_str(), vals);
	if (ierr == 0) {
	    ierr = searchSortedICC(vals, rng, hits);
	}
	else {
	    ierr = searchSortedOOCC<unsigned char>(dfname.c_str(), rng, hits);
	}
	break;}
    case ibis::SHORT: {
	array_t<int16_t> vals;
	ierr = ibis::fileManager::instance().getFile(dfname.c_str(), vals);
	if (ierr == 0) {
	    ierr = searchSortedICC(vals, rng, hits);
	}
	else {
	    ierr = searchSortedOOCC<int16_t>(dfname.c_str(), rng, hits);
	}
	break;}
    case ibis::USHORT: {
	array_t<uint16_t> vals;
	ierr = ibis::fileManager::instance().getFile(dfname.c_str(), vals);
	if (ierr == 0) {
	    ierr = searchSortedICC(vals, rng, hits);
	}
	else {
	    ierr = searchSortedOOCC<uint16_t>(dfname.c_str(), rng, hits);
	}
	break;}
    case ibis::INT: {
	array_t<int32_t> vals;
	ierr = ibis::fileManager::instance().getFile(dfname.c_str(), vals);
	if (ierr == 0) {
	    ierr = searchSortedICC(vals, rng, hits);
	}
	else {
	    ierr = searchSortedOOCC<int32_t>(dfname.c_str(), rng, hits);
	}
	break;}
    case ibis::UINT: {
	array_t<uint32_t> vals;
	ierr = ibis::fileManager::instance().getFile(dfname.c_str(), vals);
	if (ierr == 0) {
	    ierr = searchSortedICC(vals, rng, hits);
	}
	else {
	    ierr = searchSortedOOCC<uint32_t>(dfname.c_str(), rng, hits);
	}
	break;}
    case ibis::LONG: {
	array_t<int64_t> vals;
	ierr = ibis::fileManager::instance().getFile(dfname.c_str(), vals);
	if (ierr == 0) {
	    ierr = searchSortedICC(vals, rng, hits);
	}
	else {
	    ierr = searchSortedOOCC<int64_t>(dfname.c_str(), rng, hits);
	}
	break;}
    case ibis::ULONG: {
	array_t<uint64_t> vals;
	ierr = ibis::fileManager::instance().getFile(dfname.c_str(), vals);
	if (ierr == 0) {
	    ierr = searchSortedICC(vals, rng, hits);
	}
	else {
	    ierr = searchSortedOOCC<uint64_t>(dfname.c_str(), rng, hits);
	}
	break;}
    case ibis::FLOAT: {
	array_t<float> vals;
	ierr = ibis::fileManager::instance().getFile(dfname.c_str(), vals);
	if (ierr == 0) {
	    ierr = searchSortedICC(vals, rng, hits);
	}
	else {
	    ierr = searchSortedOOCC<float>(dfname.c_str(), rng, hits);
	}
	break;}
    case ibis::DOUBLE: {
	array_t<double> vals;
	ierr = ibis::fileManager::instance().getFile(dfname.c_str(), vals);
	if (ierr == 0) {
	    ierr = searchSortedICC(vals, rng, hits);
	}
	else {
	    ierr = searchSortedOOCC<double>(dfname.c_str(), rng, hits);
	}
	break;}
    default: {
	LOGGER(ibis::gVerbose > 0)
	    << "Warning -- column[" << (thePart ? thePart->name() : "?") << '.'
	    << m_name << "]::searchSorted(" << rng
	    << ") does not yet support column type "
	    << ibis::TYPESTRING[(int)m_type];
	ierr = -5;
	break;}
    } // switch (m_type)
    return (ierr < 0 ? ierr : 0);
} // ibis::column::searchSorted

int ibis::column::searchSorted(const ibis::qDiscreteRange& rng,
			       ibis::bitvector& hits) const {
    std::string dfname;
    if (dataFileName(dfname) == 0) {
	LOGGER(ibis::gVerbose >= 0)
	    << "Warning -- column[" << (thePart ? thePart->name() : "?") << '.'
	    << m_name << "]::searchSorted(" << rng.colName()
	    << "IN ...) failed to determine the data file name";
	return -4;
    }

    int ierr;
    switch (m_type) {
    case ibis::BYTE: {
	array_t<char> vals;
	ierr = ibis::fileManager::instance().getFile(dfname.c_str(), vals);
	if (ierr == 0) {
	    ierr = searchSortedICD(vals, rng, hits);
	}
	else {
	    ierr = searchSortedOOCD<char>(dfname.c_str(), rng, hits);
	}
	break;}
    case ibis::UBYTE: {
	array_t<unsigned char> vals;
	ierr = ibis::fileManager::instance().getFile(dfname.c_str(), vals);
	if (ierr == 0) {
	    ierr = searchSortedICD(vals, rng, hits);
	}
	else {
	    ierr = searchSortedOOCD<unsigned char>(dfname.c_str(), rng, hits);
	}
	break;}
    case ibis::SHORT: {
	array_t<int16_t> vals;
	ierr = ibis::fileManager::instance().getFile(dfname.c_str(), vals);
	if (ierr == 0) {
	    ierr = searchSortedICD(vals, rng, hits);
	}
	else {
	    ierr = searchSortedOOCD<int16_t>(dfname.c_str(), rng, hits);
	}
	break;}
    case ibis::USHORT: {
	array_t<uint16_t> vals;
	ierr = ibis::fileManager::instance().getFile(dfname.c_str(), vals);
	if (ierr == 0) {
	    ierr = searchSortedICD(vals, rng, hits);
	}
	else {
	    ierr = searchSortedOOCD<uint16_t>(dfname.c_str(), rng, hits);
	}
	break;}
    case ibis::INT: {
	array_t<int32_t> vals;
	ierr = ibis::fileManager::instance().getFile(dfname.c_str(), vals);
	if (ierr == 0) {
	    ierr = searchSortedICD(vals, rng, hits);
	}
	else {
	    ierr = searchSortedOOCD<int32_t>(dfname.c_str(), rng, hits);
	}
	break;}
    case ibis::UINT: {
	array_t<uint32_t> vals;
	ierr = ibis::fileManager::instance().getFile(dfname.c_str(), vals);
	if (ierr == 0) {
	    ierr = searchSortedICD(vals, rng, hits);
	}
	else {
	    ierr = searchSortedOOCD<uint32_t>(dfname.c_str(), rng, hits);
	}
	break;}
    case ibis::LONG: {
	array_t<int64_t> vals;
	ierr = ibis::fileManager::instance().getFile(dfname.c_str(), vals);
	if (ierr == 0) {
	    ierr = searchSortedICD(vals, rng, hits);
	}
	else {
	    ierr = searchSortedOOCD<int64_t>(dfname.c_str(), rng, hits);
	}
	break;}
    case ibis::ULONG: {
	array_t<uint64_t> vals;
	ierr = ibis::fileManager::instance().getFile(dfname.c_str(), vals);
	if (ierr == 0) {
	    ierr = searchSortedICD(vals, rng, hits);
	}
	else {
	    ierr = searchSortedOOCD<uint64_t>(dfname.c_str(), rng, hits);
	}
	break;}
    case ibis::FLOAT: {
	array_t<float> vals;
	ierr = ibis::fileManager::instance().getFile(dfname.c_str(), vals);
	if (ierr == 0) {
	    ierr = searchSortedICD(vals, rng, hits);
	}
	else {
	    ierr = searchSortedOOCD<float>(dfname.c_str(), rng, hits);
	}
	break;}
    case ibis::DOUBLE: {
	array_t<double> vals;
	ierr = ibis::fileManager::instance().getFile(dfname.c_str(), vals);
	if (ierr == 0) {
	    ierr = searchSortedICD(vals, rng, hits);
	}
	else {
	    ierr = searchSortedOOCD<double>(dfname.c_str(), rng, hits);
	}
	break;}
    default: {
	LOGGER(ibis::gVerbose > 0)
	    << "Warning -- column[" << (thePart ? thePart->name() : "?") << '.'
	    << m_name << "]::searchSorted(" << rng.colName() << " IN ...) "
	    << "does not yet support column type "
	    << ibis::TYPESTRING[(int)m_type];
	ierr = -5;
	break;}
    } // switch (m_type)
    return (ierr < 0 ? ierr : 0);
} // ibis::column::searchSorted

template<typename T>
int ibis::column::searchSortedICC(const array_t<T>& vals,
				  const ibis::qContinuousRange& rng,
				  ibis::bitvector& hits) const {
    hits.clear();
    uint32_t iloc, jloc;
    T ival = (rng.leftOperator() == ibis::qExpr::OP_UNDEFINED ? 0 :
	      static_cast<T>(rng.leftBound()));
    if (rng.leftOperator() == ibis::qExpr::OP_LE ||
	rng.leftOperator() == ibis::qExpr::OP_GT)
	ibis::util::round_up(rng.leftBound(), ival);
    T jval = (rng.rightOperator() == ibis::qExpr::OP_UNDEFINED ? 0 :
	      static_cast<T>(rng.rightBound()));
    if (rng.rightOperator() == ibis::qExpr::OP_GE ||
	rng.rightOperator() == ibis::qExpr::OP_LT)
	ibis::util::round_up(rng.rightBound(), jval);

    switch (rng.leftOperator()) {
    case ibis::qExpr::OP_LT: {
	switch (rng.rightOperator()) {
	case ibis::qExpr::OP_LT: {
	    if (ival < jval) {
		iloc = vals.find_upper(ival);
		jloc = vals.find(jval);
		if (iloc < jloc) {
		    hits.set(0, iloc);
		    hits.adjustSize(jloc, vals.size());
		}
	    }
	    else {
		hits.set(0, vals.size());
	    }
	    break;}
	case ibis::qExpr::OP_LE: {
	    if (ival < jval) {
		iloc = vals.find_upper(ival);
		jloc = vals.find_upper(jval);
		if (iloc < jloc) {
		    hits.set(0, iloc);
		    hits.adjustSize(jloc, vals.size());
		}
	    }
	    else {
		hits.set(0, vals.size());
	    }
	    break;}
	case ibis::qExpr::OP_GT: {
	    if (ival >= jval) {
		iloc = vals.find_upper(ival);
		if (iloc < vals.size()) {
		    hits.appendFill(0, iloc);
		    hits.adjustSize(vals.size(), vals.size());
		}
		else {
		    hits.set(0, vals.size());
		}
	    }
	    else {
		iloc = vals.find_upper(jval);
		if (iloc < vals.size()) {
		    hits.set(0, iloc);
		    hits.adjustSize(vals.size(), vals.size());
		}
		else {
		    hits.set(0, vals.size());
		}
	    }
	    break;}
	case ibis::qExpr::OP_GE: {
	    if (ival >= jval) {
		iloc = vals.find_upper(ival);
		if (iloc < vals.size()) {
		    hits.set(0, iloc);
		    hits.adjustSize(vals.size(), vals.size());
		}
		else {
		    hits.set(0, vals.size());
		}
	    }
	    else {
		iloc = vals.find(jval);
		if (iloc < vals.size()) {
		    hits.set(0, iloc);
		    hits.adjustSize(vals.size(), vals.size());
		}
		else {
		    hits.set(0, vals.size());
		}
	    }
	    break;}
	case ibis::qExpr::OP_EQ: {
	    if (rng.rightBound() > rng.leftBound()) {
		iloc = vals.find(jval);
		if (iloc < vals.size() && vals[iloc] == rng.rightBound()) {
		    for (jloc = iloc+1;
			 jloc < vals.size() && vals[jloc] == vals[iloc];
			 ++ jloc);
		    hits.set(0, iloc);
		    hits.adjustSize(jloc, vals.size());
		}
		else {
		    hits.set(0, vals.size());
		}
	    }
	    else {
		hits.set(0, vals.size());
	    }
	    break;}
	case ibis::qExpr::OP_UNDEFINED:
	default: {
	    iloc = vals.find_upper(ival);
	    if (iloc < vals.size()) {
		hits.set(0, iloc);
		hits.adjustSize(vals.size(), vals.size());
	    }
	    else {
		hits.set(0, vals.size());
	    }
	    break;}
	} // switch (rng.rightOperator())
	break;}
    case ibis::qExpr::OP_LE: {
	switch (rng.rightOperator()) {
	case ibis::qExpr::OP_LT: {
	    if (ival < jval) {
		iloc = vals.find(ival);
		jloc = vals.find(jval);
		if (iloc < jloc) {
		    hits.set(0, iloc);
		    hits.adjustSize(jloc, vals.size());
		}
	    }
	    else {
		hits.set(0, vals.size());
	    }
	    break;}
	case ibis::qExpr::OP_LE: {
	    if (ival <= jval) {
		iloc = vals.find(ival);
		jloc = vals.find_upper(jval);
		if (iloc < jloc) {
		    hits.set(0, iloc);
		    hits.adjustSize(jloc, vals.size());
		}
	    }
	    else {
		hits.set(0, vals.size());
	    }
	    break;}
	case ibis::qExpr::OP_GT: {
	    if (ival > jval) {
		iloc = vals.find(ival);
		if (iloc < vals.size()) {
		    hits.appendFill(0, iloc);
		    hits.adjustSize(vals.size(), vals.size());
		}
		else {
		    hits.set(0, vals.size());
		}
	    }
	    else {
		iloc = vals.find_upper(jval);
		if (iloc < vals.size()) {
		    hits.set(0, iloc);
		    hits.adjustSize(vals.size(), vals.size());
		}
		else {
		    hits.set(0, vals.size());
		}
	    }
	    break;}
	case ibis::qExpr::OP_GE: {
	    if (ival >= jval) {
		iloc = vals.find(ival);
		if (iloc < vals.size()) {
		    hits.set(0, iloc);
		    hits.adjustSize(vals.size(), vals.size());
		}
		else {
		    hits.set(0, vals.size());
		}
	    }
	    else {
		iloc = vals.find(jval);
		if (iloc < vals.size()) {
		    hits.set(0, iloc);
		    hits.adjustSize(vals.size(), vals.size());
		}
		else {
		    hits.set(0, vals.size());
		}
	    }
	    break;}
	case ibis::qExpr::OP_EQ: {
	    if (rng.rightBound() >= rng.leftBound()) {
		iloc = vals.find(jval);
		if (iloc < vals.size() && vals[iloc] == rng.rightBound()) {
		    for (jloc = iloc+1;
			 jloc < vals.size() && vals[jloc] == vals[iloc];
			 ++ jloc);
		    hits.set(0, iloc);
		    hits.adjustSize(jloc, vals.size());
		}
		else {
		    hits.set(0, vals.size());
		}
	    }
	    else {
		hits.set(0, vals.size());
	    }
	    break;}
	case ibis::qExpr::OP_UNDEFINED:
	default: {
	    iloc = vals.find(ival);
	    if (iloc < vals.size()) {
		hits.set(0, iloc);
		hits.adjustSize(vals.size(), vals.size());
	    }
	    else {
		hits.set(0, vals.size());
	    }
	    break;}
	} // switch (rng.rightOperator())
	break;}
    case ibis::qExpr::OP_GT: {
	switch (rng.rightOperator()) {
	case ibis::qExpr::OP_LT: {
	    if (ival <= jval) {
		iloc = vals.find(ival);
		if (iloc > 0) {
		    hits.adjustSize(iloc, vals.size());
		}
		else {
		    hits.set(0, vals.size());
		}
	    }
	    else {
		iloc = vals.find(jval);
		if (iloc > 0) {
		    hits.adjustSize(iloc, vals.size());
		}
		else {
		    hits.set(0, vals.size());
		}
	    }
	    break;}
	case ibis::qExpr::OP_LE: {
	    if (ival < jval) {
		iloc = vals.find(ival);
		if (iloc > 0) {
		    hits.adjustSize(iloc, vals.size());
		}
		else {
		    hits.set(0, vals.size());
		}
	    }
	    else {
		iloc = vals.find_upper(jval);
		if (iloc > 0) {
		    hits.adjustSize(iloc, vals.size());
		}
		else {
		    hits.set(0, vals.size());
		}
	    }
	    break;}
	case ibis::qExpr::OP_GT: {
	    if (jval < ival) {
		iloc = vals.find_upper(jval);
		jloc = vals.find(ival);
		if (jloc > iloc) {
		    hits.set(0, iloc);
		    hits.adjustSize(jloc, vals.size());
		}
		else {
		    hits.set(0, vals.size());
		}
	    }
	    else {
		hits.set(0, vals.size());
	    }
	    break;}
	case ibis::qExpr::OP_GE: {
	    if (jval < ival) {
		iloc = vals.find(jval);
		jloc = vals.find(ival);
		if (jloc > iloc) {
		    hits.set(0, iloc);
		    hits.adjustSize(jloc, vals.size());
		}
		else {
		    hits.set(0, vals.size());
		}
	    }
	    else {
		hits.set(0, vals.size());
	    }
	    break;}
	case ibis::qExpr::OP_EQ: {
	    if (rng.rightBound() > rng.leftBound()) {
		iloc = vals.find(jval);
		if (iloc < vals.size() && vals[iloc] == rng.rightBound()) {
		    jloc = vals.find_upper(jval);
		    hits.set(0, iloc);
		    hits.adjustSize(jloc, vals.size());
		}
		else {
		    hits.set(0, vals.size());
		}
	    }
	    else {
		hits.set(0, vals.size());
	    }
	    break;}
	case ibis::qExpr::OP_UNDEFINED:
	default: {
	    iloc = vals.find(ival);
	    hits.adjustSize(iloc, vals.size());
	    break;}
	} // switch (rng.rightOperator())
	break;}
    case ibis::qExpr::OP_GE: {
	switch (rng.rightOperator()) {
	case ibis::qExpr::OP_LT: {
	    if (ival < jval) {
		iloc = vals.find_upper(ival);
		if (iloc > 0) {
		    hits.adjustSize(iloc, vals.size());
		}
		else {
		    hits.set(0, vals.size());
		}
	    }
	    else {
		iloc = vals.find(jval);
		if (iloc > 0) {
		    hits.adjustSize(iloc, vals.size());
		}
		else {
		    hits.set(0, vals.size());
		}
	    }
	    break;}
	case ibis::qExpr::OP_LE: {
	    if (ival <= jval) {
		iloc = vals.find_upper(ival);
		if (iloc > 0) {
		    hits.adjustSize(iloc, vals.size());
		}
		else {
		    hits.set(0, vals.size());
		}
	    }
	    else {
		iloc = vals.find_upper(jval);
		if (iloc > 0) {
		    hits.adjustSize(iloc, vals.size());
		}
		else {
		    hits.set(0, vals.size());
		}
	    }
	    break;}
	case ibis::qExpr::OP_GT: {
	    if (jval < ival) {
		iloc = vals.find_upper(jval);
		jloc = vals.find_upper(ival);
		if (jloc > iloc) {
		    hits.set(0, iloc);
		    hits.adjustSize(jloc, vals.size());
		}
		else {
		    hits.set(0, vals.size());
		}
	    }
	    else {
		hits.set(0, vals.size());
	    }
	    break;}
	case ibis::qExpr::OP_GE: {
	    if (jval <= ival) {
		iloc = vals.find(jval);
		jloc = vals.find_upper(ival);
		if (jloc > iloc) {
		    hits.set(0, iloc);
		    hits.adjustSize(jloc, vals.size());
		}
		else {
		    hits.set(0, vals.size());
		}
	    }
	    else {
		hits.set(0, vals.size());
	    }
	    break;}
	case ibis::qExpr::OP_EQ: {
	    if (rng.rightBound() >= rng.leftBound()) {
		iloc = vals.find(jval);
		if (iloc < vals.size() && vals[iloc] == rng.rightBound()) {
		    jloc = vals.find_upper(jval);
		    hits.set(0, iloc);
		    hits.adjustSize(jloc, vals.size());
		}
		else {
		    hits.set(0, vals.size());
		}
	    }
	    else {
		hits.set(0, vals.size());
	    }
	    break;}
	case ibis::qExpr::OP_UNDEFINED:
	default: {
	    iloc = vals.find_upper(ival);
	    hits.adjustSize(iloc, vals.size());
	    break;}
	} // switch (rng.rightOperator())
	break;}
    case ibis::qExpr::OP_EQ: {
	switch (rng.rightOperator()) {
	case ibis::qExpr::OP_LT: {
	    if (rng.leftBound() < rng.rightBound()) {
		iloc = vals.find(ival);
		if (iloc < vals.size() && vals[iloc] == rng.leftBound()) {
		    jloc = vals.find_upper(ival);
		    hits.set(0, iloc);
		    hits.adjustSize(jloc, vals.size());
		}
		else {
		    hits.set(0, vals.size());
		}
	    }
	    else {
		hits.set(0, vals.size());
	    }
	    break;}
	case ibis::qExpr::OP_LE: {
	    if (rng.leftBound() <= rng.rightBound()) {
		iloc = vals.find(ival);
		if (iloc < vals.size() && vals[iloc] == rng.leftBound()) {
		    jloc = vals.find_upper(ival);
		    hits.set(0, iloc);
		    hits.adjustSize(jloc, vals.size());
		}
		else {
		    hits.set(0, vals.size());
		}
	    }
	    else {
		hits.set(0, vals.size());
	    }
	    break;}
	case ibis::qExpr::OP_GT: {
	    if (rng.leftBound() > rng.rightBound()) {
		iloc = vals.find(ival);
		if (iloc < vals.size() && vals[iloc] == rng.leftBound()) {
		    jloc = vals.find_upper(ival);
		    hits.set(0, iloc);
		    hits.adjustSize(jloc, vals.size());
		}
		else {
		    hits.set(0, vals.size());
		}
	    }
	    else {
		hits.set(0, vals.size());
	    }
	    break;}
	case ibis::qExpr::OP_GE: {
	    if (rng.leftBound() >= rng.rightBound()) {
		iloc = vals.find(ival);
		if (iloc < vals.size() && vals[iloc] == rng.leftBound()) {
		    jloc = vals.find_upper(ival);
		    hits.set(0, iloc);
		    hits.adjustSize(jloc, vals.size());
		}
		else {
		    hits.set(0, vals.size());
		}
	    }
	    else {
		hits.set(0, vals.size());
	    }
	    break;}
	case ibis::qExpr::OP_EQ: {
	    if (rng.leftBound() == rng.rightBound()) {
		iloc = vals.find(ival);
		if (iloc < vals.size() && vals[iloc] == rng.leftBound()) {
		    jloc = vals.find_upper(ival);
		    hits.set(0, iloc);
		    hits.adjustSize(jloc, vals.size());
		}
		else {
		    hits.set(0, vals.size());
		}
	    }
	    else {
		hits.set(0, vals.size());
	    }
	    break;}
	case ibis::qExpr::OP_UNDEFINED:
	default: {
	    iloc = vals.find(ival);
	    if (iloc < vals.size() && vals[iloc] == rng.leftBound()) {
		jloc = vals.find_upper(ival);
		hits.set(0, iloc);
		hits.adjustSize(jloc, vals.size());
	    }
	    else {
		hits.set(0, vals.size());
	    }
	    break;}
	} // switch (rng.rightOperator())
	break;}
    case ibis::qExpr::OP_UNDEFINED:
    default: {
	switch (rng.rightOperator()) {
	case ibis::qExpr::OP_LT: {
	    jloc = vals.find(jval);
	    hits.adjustSize(jloc, vals.size());
	    break;}
	case ibis::qExpr::OP_LE: {
	    jloc = vals.find_upper(jval);
	    hits.adjustSize(jloc, vals.size());
	    break;}
	case ibis::qExpr::OP_GT: {
	    jloc = vals.find_upper(jval);
	    if (jloc < vals.size()) {
		hits.set(0, jloc);
		hits.adjustSize(vals.size(), vals.size());
	    }
	    else {
		hits.set(0, vals.size());
	    }
	    break;}
	case ibis::qExpr::OP_GE: {
	    jloc = vals.find(jval);
	    if (jloc < vals.size()) {
		hits.set(0, jloc);
		hits.adjustSize(vals.size(), vals.size());
	    }
	    else {
		hits.set(0, vals.size());
	    }
	    break;}
	case ibis::qExpr::OP_EQ: {
	    iloc = vals.find(jval);
	    if (iloc < vals.size() && vals[iloc] == rng.rightBound()) {
		jloc = vals.find_upper(jval);
		hits.set(0, iloc);
		hits.adjustSize(jloc, vals.size());
	    }
	    else {
		hits.set(0, vals.size());
	    }
	    break;}
	case ibis::qExpr::OP_UNDEFINED:
	default: {
	    hits.set(0, vals.size());
	    return -8;
	    break;}
	} // switch (rng.rightOperator())
	break;}
    } // switch (rng.leftOperator())
    return 0;
} // ibis::column::searchSortedICC

/// The backup option for searchSortedIC.  This function opens the named
/// file and reads its content one word at a time, which is likely to be
/// very slow.  It does assume the content of the file is sorted in
/// ascending order and perform binary searches.
template<typename T>
int ibis::column::searchSortedOOCC(const char* fname,
				   const ibis::qContinuousRange& rng,
				   ibis::bitvector& hits) const {
    int fdes = UnixOpen(fname, OPEN_READONLY);
    if (fdes < 0) {
	LOGGER(ibis::gVerbose >= 0)
	    << "Warning -- column[" << (thePart ? thePart->name() : "?") << '.'
	    << m_name << "]::searchSortedOOCC<" << typeid(T).name() << ">("
	    << fname << ", " << rng
	    << ") failed to open the named data file, errno = " << errno
	    << strerror(errno);
	return -1;
    }
#if defined(_WIN32) && defined(_MSC_VER)
    (void)_setmode(fdes, _O_BINARY);
#endif

    int ierr = UnixSeek(fdes, 0, SEEK_END);
    if (ierr < 0) {
	LOGGER(ibis::gVerbose >= 0)
	    << "Warning -- column[" << (thePart ? thePart->name() : "?") << '.'
	    << m_name << "]::searchSortedOOCC<" << typeid(T).name() << ">("
	    << fname << ", " << rng << ") failed to seek to the end of file";
	(void) UnixClose(fdes);
	return -2;
    }
    const uint32_t nrows = ierr / sizeof(T);
    const uint32_t sz = sizeof(T);
    hits.clear();
    uint32_t iloc, jloc;
    T ival = (rng.leftOperator() == ibis::qExpr::OP_UNDEFINED ? 0 :
	      static_cast<T>(rng.leftBound()));
    if (rng.leftOperator() == ibis::qExpr::OP_LE ||
	rng.leftOperator() == ibis::qExpr::OP_GT)
	ibis::util::round_up(rng.leftBound(), ival);

    T jval = (rng.rightOperator() == ibis::qExpr::OP_UNDEFINED ? 0 :
	      static_cast<T>(rng.rightBound()));
    if (rng.rightOperator() == ibis::qExpr::OP_GE ||
	rng.rightOperator() == ibis::qExpr::OP_LT)
	ibis::util::round_up(rng.rightBound(), jval);

    switch (rng.leftOperator()) {
    case ibis::qExpr::OP_LT: {
	switch (rng.rightOperator()) {
	case ibis::qExpr::OP_LT: {
	    if (ival < jval) {
		iloc = findUpper<T>(fdes, nrows, ival);
		jloc = findLower<T>(fdes, nrows, jval);
		if (iloc < jloc) {
		    hits.set(0, iloc);
		    hits.adjustSize(jloc, nrows);
		}
	    }
	    else {
		hits.set(0, nrows);
	    }
	    break;}
	case ibis::qExpr::OP_LE: {
	    if (ival < jval) {
		iloc = findUpper<T>(fdes, nrows, ival);
		jloc = findUpper<T>(fdes, nrows, jval);
		if (iloc < jloc) {
		    hits.set(0, iloc);
		    hits.adjustSize(jloc, nrows);
		}
	    }
	    else {
		hits.set(0, nrows);
	    }
	    break;}
	case ibis::qExpr::OP_GT: {
	    if (ival >= jval) {
		iloc = findUpper<T>(fdes, nrows, ival);
		if (iloc < nrows) {
		    hits.appendFill(0, iloc);
		    hits.adjustSize(nrows, nrows);
		}
		else {
		    hits.set(0, nrows);
		}
	    }
	    else {
		iloc = findUpper<T>(fdes, nrows, jval);
		if (iloc < nrows) {
		    hits.set(0, iloc);
		    hits.adjustSize(nrows, nrows);
		}
		else {
		    hits.set(0, nrows);
		}
	    }
	    break;}
	case ibis::qExpr::OP_GE: {
	    if (ival >= jval) {
		iloc = findUpper<T>(fdes, nrows, ival);
		if (iloc < nrows) {
		    hits.set(0, iloc);
		    hits.adjustSize(nrows, nrows);
		}
		else {
		    hits.set(0, nrows);
		}
	    }
	    else {
		iloc = findLower<T>(fdes, nrows, jval);
		if (iloc < nrows) {
		    hits.set(0, iloc);
		    hits.adjustSize(nrows, nrows);
		}
		else {
		    hits.set(0, nrows);
		}
	    }
	    break;}
	case ibis::qExpr::OP_EQ: {
	    if (rng.rightBound() > rng.leftBound()) {
		T tmp;
		iloc = findLower<T>(fdes, nrows, jval);
		ierr = UnixSeek(fdes, iloc*sz, SEEK_SET);
		ierr = UnixRead(fdes, &tmp, sz);
		if (iloc < nrows && ierr == (int) sz &&
		    tmp == rng.rightBound()) {
		    for (jloc = iloc+1; jloc < nrows; ++ jloc) {
			ierr = UnixRead(fdes, &tmp, sz);
			if (ierr < (int)sz || tmp != jval) break;
		    }
		    hits.set(0, iloc);
		    hits.adjustSize(jloc, nrows);
		    ibis::fileManager::instance().recordPages(iloc*sz,
							      jloc*sz+sz);
		}
		else {
		    hits.set(0, nrows);
		    ibis::fileManager::instance().recordPages(iloc*sz,
							      iloc*sz+sz);
		}
	    }
	    else {
		hits.set(0, nrows);
	    }
	    break;}
	case ibis::qExpr::OP_UNDEFINED:
	default: {
	    iloc = findUpper<T>(fdes, nrows, ival);
	    if (iloc < nrows) {
		hits.set(0, iloc);
		hits.adjustSize(nrows, nrows);
	    }
	    else {
		hits.set(0, nrows);
	    }
	    break;}
	} // switch (rng.rightOperator())
	break;}
    case ibis::qExpr::OP_LE: {
	switch (rng.rightOperator()) {
	case ibis::qExpr::OP_LT: {
	    if (ival < jval) {
		iloc = findLower<T>(fdes, nrows, ival);
		jloc = findLower<T>(fdes, nrows, jval);
		if (iloc < jloc) {
		    hits.set(0, iloc);
		    hits.adjustSize(jloc, nrows);
		}
	    }
	    else {
		hits.set(0, nrows);
	    }
	    break;}
	case ibis::qExpr::OP_LE: {
	    if (ival <= jval) {
		iloc = findLower<T>(fdes, nrows, ival);
		jloc = findUpper<T>(fdes, nrows, jval);
		if (iloc < jloc) {
		    hits.set(0, iloc);
		    hits.adjustSize(jloc, nrows);
		}
	    }
	    else {
		hits.set(0, nrows);
	    }
	    break;}
	case ibis::qExpr::OP_GT: {
	    if (ival > jval) {
		iloc = findLower<T>(fdes, nrows, ival);
		if (iloc < nrows) {
		    hits.appendFill(0, iloc);
		    hits.adjustSize(nrows, nrows);
		}
		else {
		    hits.set(0, nrows);
		}
	    }
	    else {
		iloc = findUpper<T>(fdes, nrows, jval);
		if (iloc < nrows) {
		    hits.set(0, iloc);
		    hits.adjustSize(nrows, nrows);
		}
		else {
		    hits.set(0, nrows);
		}
	    }
	    break;}
	case ibis::qExpr::OP_GE: {
	    if (ival >= jval) {
		iloc = findLower<T>(fdes, nrows, ival);
		if (iloc < nrows) {
		    hits.set(0, iloc);
		    hits.adjustSize(nrows, nrows);
		}
		else {
		    hits.set(0, nrows);
		}
	    }
	    else {
		iloc = findLower<T>(fdes, nrows, jval);
		if (iloc < nrows) {
		    hits.set(0, iloc);
		    hits.adjustSize(nrows, nrows);
		}
		else {
		    hits.set(0, nrows);
		}
	    }
	    break;}
	case ibis::qExpr::OP_EQ: {
	    if (rng.rightBound() >= rng.leftBound()) {
		T tmp;
		iloc = findLower<T>(fdes, nrows, jval);
		ierr = UnixSeek(fdes, iloc*sz, SEEK_SET);
		ierr = UnixRead(fdes, &tmp, sz);
		if (iloc < nrows && ierr == (int)sz &&
		    tmp == rng.rightBound()) {
		    for (jloc = iloc+1; jloc < nrows; ++ jloc) {
			ierr = UnixRead(fdes, &tmp, sz);
			if (ierr < (int) sz || tmp != jval) break;
		    }
		    hits.set(0, iloc);
		    hits.adjustSize(jloc, nrows);
		    ibis::fileManager::instance().recordPages(iloc*sz,
							      jloc*sz+sz);
		}
		else {
		    hits.set(0, nrows);
		    ibis::fileManager::instance().recordPages(iloc*sz,
							      iloc*sz+sz);
		}
	    }
	    else {
		hits.set(0, nrows);
	    }
	    break;}
	case ibis::qExpr::OP_UNDEFINED:
	default: {
	    iloc = findLower<T>(fdes, nrows, ival);
	    if (iloc < nrows) {
		hits.set(0, iloc);
		hits.adjustSize(nrows, nrows);
	    }
	    else {
		hits.set(0, nrows);
	    }
	    break;}
	} // switch (rng.rightOperator())
	break;}
    case ibis::qExpr::OP_GT: {
	switch (rng.rightOperator()) {
	case ibis::qExpr::OP_LT: {
	    if (ival <= jval) {
		iloc = findLower<T>(fdes, nrows, ival);
		if (iloc > 0) {
		    hits.adjustSize(iloc, nrows);
		}
		else {
		    hits.set(0, nrows);
		}
	    }
	    else {
		iloc = findLower<T>(fdes, nrows, jval);
		if (iloc > 0) {
		    hits.adjustSize(iloc, nrows);
		}
		else {
		    hits.set(0, nrows);
		}
	    }
	    break;}
	case ibis::qExpr::OP_LE: {
	    if (ival < jval) {
		iloc = findLower<T>(fdes, nrows, ival);
		if (iloc > 0) {
		    hits.adjustSize(iloc, nrows);
		}
		else {
		    hits.set(0, nrows);
		}
	    }
	    else {
		iloc = findUpper<T>(fdes, nrows, jval);
		if (iloc > 0) {
		    hits.adjustSize(iloc, nrows);
		}
		else {
		    hits.set(0, nrows);
		}
	    }
	    break;}
	case ibis::qExpr::OP_GT: {
	    if (jval < ival) {
		iloc = findUpper<T>(fdes, nrows, jval);
		jloc = findLower<T>(fdes, nrows, ival);
		if (jloc > iloc) {
		    hits.set(0, iloc);
		    hits.adjustSize(jloc, nrows);
		}
		else {
		    hits.set(0, nrows);
		}
	    }
	    else {
		hits.set(0, nrows);
	    }
	    break;}
	case ibis::qExpr::OP_GE: {
	    if (jval < ival) {
		iloc = findLower<T>(fdes, nrows, jval);
		jloc = findLower<T>(fdes, nrows, ival);
		if (jloc > iloc) {
		    hits.set(0, iloc);
		    hits.adjustSize(jloc, nrows);
		}
		else {
		    hits.set(0, nrows);
		}
	    }
	    else {
		hits.set(0, nrows);
	    }
	    break;}
	case ibis::qExpr::OP_EQ: {
	    if (rng.rightBound() > rng.leftBound()) {
		T tmp;
		iloc = findLower<T>(fdes, nrows, jval);
		ierr = UnixSeek(fdes, iloc*sz, SEEK_SET);
		ierr = UnixRead(fdes, &tmp, sz);
		if (iloc < nrows && ierr == (int)sz &&
		    tmp == rng.rightBound()) {
		    for (jloc = iloc+1; jloc < nrows; ++ jloc) {
			ierr = UnixRead(fdes, &tmp, sz);
			if (ierr < (int)sz || tmp != jval) break;
		    }
		    hits.set(0, iloc);
		    hits.adjustSize(jloc, nrows);
		    ibis::fileManager::instance().recordPages(iloc*sz,
							      jloc*sz+sz);
		}
		else {
		    hits.set(0, nrows);
		    ibis::fileManager::instance().recordPages(iloc*sz,
							      iloc*sz+sz);
		}
	    }
	    else {
		hits.set(0, nrows);
	    }
	    break;}
	case ibis::qExpr::OP_UNDEFINED:
	default: {
	    iloc = findLower<T>(fdes, nrows, ival);
	    hits.adjustSize(iloc, nrows);
	    break;}
	} // switch (rng.rightOperator())
	break;}
    case ibis::qExpr::OP_GE: {
	switch (rng.rightOperator()) {
	case ibis::qExpr::OP_LT: {
	    if (ival < jval) {
		iloc = findUpper<T>(fdes, nrows, ival);
		if (iloc > 0) {
		    hits.adjustSize(iloc, nrows);
		}
		else {
		    hits.set(0, nrows);
		}
	    }
	    else {
		iloc = findLower<T>(fdes, nrows, jval);
		if (iloc > 0) {
		    hits.adjustSize(iloc, nrows);
		}
		else {
		    hits.set(0, nrows);
		}
	    }
	    break;}
	case ibis::qExpr::OP_LE: {
	    if (ival <= jval) {
		iloc = findUpper<T>(fdes, nrows, ival);
		if (iloc > 0) {
		    hits.adjustSize(iloc, nrows);
		}
		else {
		    hits.set(0, nrows);
		}
	    }
	    else {
		iloc = findUpper<T>(fdes, nrows, jval);
		if (iloc > 0) {
		    hits.adjustSize(iloc, nrows);
		}
		else {
		    hits.set(0, nrows);
		}
	    }
	    break;}
	case ibis::qExpr::OP_GT: {
	    if (jval < ival) {
		iloc = findUpper<T>(fdes, nrows, jval);
		jloc = findUpper<T>(fdes, nrows, ival);
		if (jloc > iloc) {
		    hits.set(0, iloc);
		    hits.adjustSize(jloc, nrows);
		}
		else {
		    hits.set(0, nrows);
		}
	    }
	    else {
		hits.set(0, nrows);
	    }
	    break;}
	case ibis::qExpr::OP_GE: {
	    if (jval <= ival) {
		iloc = findLower<T>(fdes, nrows, jval);
		jloc = findUpper<T>(fdes, nrows, ival);
		if (jloc > iloc) {
		    hits.set(0, iloc);
		    hits.adjustSize(jloc, nrows);
		}
		else {
		    hits.set(0, nrows);
		}
	    }
	    else {
		hits.set(0, nrows);
	    }
	    break;}
	case ibis::qExpr::OP_EQ: {
	    if (rng.rightBound() >= rng.leftBound()) {
		T tmp;
		iloc = findLower<T>(fdes, nrows, jval);
		ierr = UnixSeek(fdes, iloc*sz, SEEK_SET);
		ierr = UnixRead(fdes, &tmp, sz);
		if (iloc < nrows && ierr == (int)sz &&
		    tmp == rng.rightBound()) {
		    for (jloc = iloc+1; jloc < nrows; ++ jloc) {
			ierr = UnixRead(fdes, &tmp, sz);
			if (ierr < (int)sz || tmp != jval) break;
		    }
		    hits.set(0, iloc);
		    hits.adjustSize(jloc, nrows);
		    ibis::fileManager::instance().recordPages(iloc*sz,
							      jloc*sz+sz);
		}
		else {
		    hits.set(0, nrows);
		    ibis::fileManager::instance().recordPages(iloc*sz,
							      iloc*sz+sz);
		}
	    }
	    else {
		hits.set(0, nrows);
	    }
	    break;}
	case ibis::qExpr::OP_UNDEFINED:
	default: {
	    iloc = findUpper<T>(fdes, nrows, ival);
	    hits.adjustSize(iloc, nrows);
	    break;}
	} // switch (rng.rightOperator())
	break;}
    case ibis::qExpr::OP_EQ: {
	switch (rng.rightOperator()) {
	case ibis::qExpr::OP_LT: {
	    if (rng.leftBound() < rng.rightBound()) {
		T tmp;
		iloc = findLower<T>(fdes, nrows, ival);
		ierr = UnixSeek(fdes, iloc*sz, SEEK_SET);
		ierr = UnixRead(fdes, &tmp, sz);
		if (iloc < nrows && ierr == (int)sz &&
		    tmp == rng.leftBound()) {
		    for (jloc = iloc+1; jloc < nrows; ++ jloc) {
			ierr = UnixRead(fdes, &tmp, sz);
			if (ierr < (int)sz || tmp != ival) break;
		    }
		    hits.set(0, iloc);
		    hits.adjustSize(jloc, nrows);
		    ibis::fileManager::instance().recordPages(iloc*sz,
							      jloc*sz+sz);
		}
		else {
		    hits.set(0, nrows);
		    ibis::fileManager::instance().recordPages(iloc*sz,
							      iloc*sz+sz);
		}
	    }
	    else {
		hits.set(0, nrows);
	    }
	    break;}
	case ibis::qExpr::OP_LE: {
	    if (rng.leftBound() <= rng.rightBound()) {
		T tmp;
		iloc = findLower<T>(fdes, nrows, ival);
		ierr = UnixSeek(fdes, iloc*sz, SEEK_SET);
		ierr = UnixRead(fdes, &tmp, sz);
		if (iloc < nrows && ierr == (int)sz &&
		    tmp == rng.leftBound()) {
		    for (jloc = iloc+1; jloc < nrows; ++ jloc) {
			ierr = UnixRead(fdes, &tmp, sz);
			if (ierr < (int)sz || tmp != ival) break;
		    }
		    hits.set(0, iloc);
		    hits.adjustSize(jloc, nrows);
		    ibis::fileManager::instance().recordPages(iloc*sz,
							      jloc*sz+sz);
		}
		else {
		    hits.set(0, nrows);
		    ibis::fileManager::instance().recordPages(iloc*sz,
							      iloc*sz+sz);
		}
	    }
	    else {
		hits.set(0, nrows);
	    }
	    break;}
	case ibis::qExpr::OP_GT: {
	    if (rng.leftBound() > rng.rightBound()) {
		T tmp;
		iloc = findLower<T>(fdes, nrows, ival);
		ierr = UnixSeek(fdes, iloc*sz, SEEK_SET);
		ierr = UnixRead(fdes, &tmp, sz);
		if (iloc < nrows && ierr == (int) sz &&
		    tmp == rng.leftBound()) {
		    for (jloc = iloc+1; jloc < nrows; ++ jloc) {
			ierr = UnixRead(fdes, &tmp, sz);
			if (ierr < (int)sz || tmp != ival) break;
		    }
		    hits.set(0, iloc);
		    hits.adjustSize(jloc, nrows);
		    ibis::fileManager::instance().recordPages(iloc*sz,
							      jloc*sz+sz);
		}
		else {
		    hits.set(0, nrows);
		    ibis::fileManager::instance().recordPages(iloc*sz,
							      iloc*sz+sz);
		}
	    }
	    else {
		hits.set(0, nrows);
	    }
	    break;}
	case ibis::qExpr::OP_GE: {
	    if (rng.leftBound() >= rng.rightBound()) {
		T tmp;
		iloc = findLower<T>(fdes, nrows, ival);
		ierr = UnixSeek(fdes, iloc*sz, SEEK_SET);
		ierr = UnixRead(fdes, &tmp, sz);
		if (iloc < nrows && ierr == (int)sz &&
		    tmp == rng.leftBound()) {
		    for (jloc = iloc+1; jloc < nrows; ++ jloc) {
			ierr = UnixRead(fdes, &tmp, sz);
			if (ierr < (int)sz || tmp != ival) break;
		    }
		    hits.set(0, iloc);
		    hits.adjustSize(jloc, nrows);
		    ibis::fileManager::instance().recordPages(iloc*sz,
							      jloc*sz+sz);
		}
		else {
		    hits.set(0, nrows);
		    ibis::fileManager::instance().recordPages(iloc*sz,
							      iloc*sz+sz);
		}
	    }
	    else {
		hits.set(0, nrows);
	    }
	    break;}
	case ibis::qExpr::OP_EQ: {
	    if (rng.leftBound() == rng.rightBound()) {
		T tmp;
		iloc = findLower<T>(fdes, nrows, ival);
		ierr = UnixSeek(fdes, iloc*sz, SEEK_SET);
		ierr = UnixRead(fdes, &tmp, sz);
		if (iloc < nrows && ierr == (int)sz &&
		    tmp == rng.leftBound()) {
		    for (jloc = iloc+1; jloc < nrows; ++ jloc) {
			ierr = UnixRead(fdes, &tmp, sz);
			if (ierr < (int)sz || tmp != ival) break;
		    }
		    hits.set(0, iloc);
		    hits.adjustSize(jloc, nrows);
		    ibis::fileManager::instance().recordPages(iloc*sz,
							      jloc*sz+sz);
		}
		else {
		    hits.set(0, nrows);
		    ibis::fileManager::instance().recordPages(iloc*sz,
							      iloc*sz+sz);
		}
	    }
	    else {
		hits.set(0, nrows);
	    }
	    break;}
	case ibis::qExpr::OP_UNDEFINED:
	default: {
	    T tmp;
	    iloc = findLower<T>(fdes, nrows, ival);
	    ierr = UnixSeek(fdes, iloc*sz, SEEK_SET);
	    ierr = UnixRead(fdes, &tmp, sz);
	    if (iloc < nrows && ierr == (int)sz &&
		tmp == rng.leftBound()) {
		for (jloc = iloc+1; jloc < nrows; ++ jloc) {
		    ierr = UnixRead(fdes, &tmp, sz);
		    if (ierr < (int)sz || tmp != ival) break;
		}
		hits.set(0, iloc);
		hits.adjustSize(jloc, nrows);
		ibis::fileManager::instance().recordPages(iloc*sz, jloc*sz+sz);
	    }
	    else {
		hits.set(0, nrows);
		ibis::fileManager::instance().recordPages(iloc*sz, iloc*sz+sz);
	    }
	    break;}
	} // switch (rng.rightOperator())
	break;}
    case ibis::qExpr::OP_UNDEFINED:
    default: {
	switch (rng.rightOperator()) {
	case ibis::qExpr::OP_LT: {
	    jloc = findLower<T>(fdes, nrows, jval);
	    hits.adjustSize(jloc, nrows);
	    break;}
	case ibis::qExpr::OP_LE: {
	    jloc = findUpper<T>(fdes, nrows, jval);
	    hits.adjustSize(jloc, nrows);
	    break;}
	case ibis::qExpr::OP_GT: {
	    jloc = findUpper<T>(fdes, nrows, jval);
	    if (jloc < nrows) {
		hits.set(0, jloc);
		hits.adjustSize(nrows, nrows);
	    }
	    else {
		hits.set(0, nrows);
	    }
	    break;}
	case ibis::qExpr::OP_GE: {
	    jloc = findLower<T>(fdes, nrows, jval);
	    if (jloc < nrows) {
		hits.set(0, jloc);
		hits.adjustSize(nrows, nrows);
	    }
	    else {
		hits.set(0, nrows);
	    }
	    break;}
	case ibis::qExpr::OP_EQ: {
	    T tmp;
	    iloc = findLower<T>(fdes, nrows, jval);
	    ierr = UnixSeek(fdes, iloc*sz, SEEK_SET);
	    ierr = UnixRead(fdes, &tmp, sz);
	    if (iloc < nrows && ierr == (int)sz &&
		tmp == rng.rightBound()) {
		for (jloc = iloc+1; jloc < nrows; ++ jloc) {
		    ierr = UnixRead(fdes, &tmp, sz);
		    if (ierr < (int)sz || tmp != jval) break;
		}
		hits.set(0, iloc);
		hits.adjustSize(jloc, nrows);
		ibis::fileManager::instance().recordPages(iloc*sz, jloc*sz+sz);
	    }
	    else {
		hits.set(0, nrows);
		ibis::fileManager::instance().recordPages(iloc*sz, iloc*sz+sz);
	    }
	    break;}
	case ibis::qExpr::OP_UNDEFINED:
	default: {
	    hits.set(0, nrows);
	    return -8;
	    break;}
	} // switch (rng.rightOperator())
	break;}
    } // switch (rng.leftOperator())

    (void) UnixClose(fdes);
    return 0;
} // ibis::column::searchSortedOOCC

/// An equivalent of array_t<T>::find.  It reads the open file one word at
/// a time and therefore is likely to be very slow.
template<typename T> uint32_t
ibis::column::findLower(int fdes, const uint32_t nr, const T tgt) const {
    int ierr;
    const uint32_t sz = sizeof(T);
    uint32_t left = 0, right = nr;
    uint32_t mid = ((left + right) >> 1);
    while (mid > left) {
	off_t pos = mid * sz;
	ierr = UnixSeek(fdes, pos, SEEK_SET);
	if (ierr != pos) {
	    LOGGER(ibis::gVerbose >= 0)
		<< "Warning -- column[" << (thePart ? thePart->name() : "?")
		<< "." << m_name << "]::findLower(" << fdes << ", " << tgt
		<< ") failed to seek to " << pos << ", ierr = " << ierr;
	    return nr;
	}

	T tmp;
	ierr = UnixRead(fdes, &tmp, sz);
	ibis::fileManager::instance().recordPages(pos, pos+sz);
	if (ierr != (int) sz) {
	    LOGGER(ibis::gVerbose >= 0)
		<< "Warning -- column[" << (thePart ? thePart->name() : "?")
		<< "." << m_name << "]::findLower(" << fdes << ", " << tgt
		<< ") failed to read a word of type " << typeid(T).name()
		<< " at " << pos << ", ierr = " << ierr;
	    return nr;
	}

	if (tmp < tgt)
	    left = mid;
	else
	    right = mid;
	mid = ((left + right) >> 1);
    }

    if (mid < nr) { // read the value at mid
	off_t pos = mid * sz;
	ierr = UnixSeek(fdes, pos, SEEK_SET);
	if (ierr != pos) {
	    LOGGER(ibis::gVerbose >= 0)
		<< "Warning -- column[" << (thePart ? thePart->name() : "?")
		<< "." << m_name << "]::findLower(" << fdes << ", " << tgt
		<< ") failed to seek to " << pos << ", ierr = " << ierr;
	    return nr;
	}

	T tmp;
	ierr = UnixRead(fdes, &tmp, sz);
	ibis::fileManager::instance().recordPages(pos, pos+sz);
	if (ierr != (int) sz) {
	    LOGGER(ibis::gVerbose >= 0)
		<< "Warning -- column[" << (thePart ? thePart->name() : "?")
		<< "." << m_name << "]::findLower(" << fdes << ", " << tgt
		<< ") failed to read a word of type " << typeid(T).name()
		<< " at " << pos << ", ierr = " << ierr;
	    return nr;
	}
	if (tmp < tgt)
	    ++ mid;
    }
    return mid;
} // ibis::column::findLower

/// An equivalent of array_t<T>::find_upper.  It reads the open file one
/// word at a time and therefore is likely to be very slow.
template<typename T> uint32_t
ibis::column::findUpper(int fdes, const uint32_t nr, const T tgt) const {
    int ierr;
    const uint32_t sz = sizeof(T);
    uint32_t left = 0, right = nr;
    uint32_t mid = ((left + right) >> 1);
    while (mid > left) {
	off_t pos = mid * sz;
	ierr = UnixSeek(fdes, pos, SEEK_SET);
	if (ierr != pos) {
	    LOGGER(ibis::gVerbose >= 0)
		<< "Warning -- column[" << (thePart ? thePart->name() : "?")
		<< "." << m_name << "]::findUpper(" << fdes << ", " << tgt
		<< ") failed to seek to " << pos << ", ierr = " << ierr;
	    return nr;
	}

	T tmp;
	ierr = UnixRead(fdes, &tmp, sz);
	ibis::fileManager::instance().recordPages(pos, pos+sz);
	if (ierr != (int) sz) {
	    LOGGER(ibis::gVerbose >= 0)
		<< "Warning -- column[" << (thePart ? thePart->name() : "?")
		<< "." << m_name << "]::findUpper(" << fdes << ", " << tgt
		<< ") failed to read a word of type " << typeid(T).name()
		<< " at " << pos << ", ierr = " << ierr;
	    return nr;
	}

	if (tgt < tmp)
	    right = mid;
	else
	    left = mid;
	mid = ((left + right) >> 1);
    }

    if (mid < nr) { // read the value at mid
	off_t pos = mid * sz;
	ierr = UnixSeek(fdes, pos, SEEK_SET);
	if (ierr != pos) {
	    LOGGER(ibis::gVerbose >= 0)
		<< "Warning -- column[" << (thePart ? thePart->name() : "?")
		<< "." << m_name << "]::findLower(" << fdes << ", " << tgt
		<< ") failed to seek to " << pos << ", ierr = " << ierr;
	    return nr;
	}

	T tmp;
	ierr = UnixRead(fdes, &tmp, sz);
	ibis::fileManager::instance().recordPages(pos, pos+sz);
	if (ierr != (int) sz) {
	    LOGGER(ibis::gVerbose >= 0)
		<< "Warning -- column[" << (thePart ? thePart->name() : "?")
		<< "." << m_name << "]::findLower(" << fdes << ", " << tgt
		<< ") failed to read a word of type " << typeid(T).name()
		<< " at " << pos << ", ierr = " << ierr;
	    return nr;
	}
	if (! (tgt < tmp))
	    ++ mid;
    }
    return mid;
} // ibis::column::findUpper

template<typename T>
int ibis::column::searchSortedICD(const array_t<T>& vals,
				  const ibis::qDiscreteRange& rng,
				  ibis::bitvector& hits) const {
    const std::vector<double>& u = rng.getValues();
    std::string evt = "column::searchSortedICD";
    if (ibis::gVerbose >= 5) {
	std::ostringstream oss;
	oss << "column[" << (thePart ? thePart->name() : "?") << '.'
	    << m_name << "]::searchSortedICD<" << typeid(T).name()
	    << ">(" << rng.colName() << " IN " << u.size() << "-element list)";
	evt = oss.str();
    }
    ibis::util::timer mytimer(evt.c_str(), 5);
    hits.clear();
    hits.reserve(vals.size(), u.size()); // reserve space
    if ((size_t)(u.size()*(1.0+std::log((double)vals.size()))) >=
	(u.size()+vals.size())) {
	// go through the two lists to find matches
	LOGGER(ibis::gVerbose >= 5)
	    << evt << " will march through two sorted lists";
	size_t ju = 0;
	size_t jv = 0;
	while (ju < u.size() && jv < vals.size()) {
	    while (ju < u.size() && u[ju] < vals[jv]) ++ ju;
	    while (jv < vals.size() && u[ju] > vals[jv]) ++ jv;
	    if (u[ju] == vals[jv]) {
		hits.setBit(jv, 1);
		++ jv;
	    }
	}
    }
    else {
	// do binary search for each value in u
	LOGGER(ibis::gVerbose >= 5)
	    << evt << " will use " << u.size()
	    << " binary search" << (u.size() > 1 ? "es" : "");
	for (size_t j = 0; j < u.size(); ++ j) {
	    uint32_t jloc = vals.find(static_cast<T>(u[j]));
	    if (vals[jloc] == u[j]) {
		hits.setBit(jloc, 1);
	    }
	}
    }
    hits.adjustSize(0, vals.size());
    return 0;
} // ibis::column::searchSortedICD

/// This version of search function reads the content of data file through
/// explicit read operations.  It sequentially reads the content of the
/// data file.  Note the content of the data file is assumed to be sorted
/// in ascending order as elementary data type T.
template<typename T>
int ibis::column::searchSortedOOCD(const char* fname,
				   const ibis::qDiscreteRange& rng,
				   ibis::bitvector& hits) const {
    const std::vector<double>& u = rng.getValues();
    std::string evt = "column::searchSortedOOCD";
    if (ibis::gVerbose >= 5) {
	std::ostringstream oss;
	oss << "column[" << (thePart ? thePart->name() : "?") << '.'
	    << m_name << "]::searchSortedOOCD<" << typeid(T).name()
	    << ">(" << fname << ", " << rng.colName() << " IN "
	    << u.size() << "-element list)";
	evt = oss.str();
    }
    ibis::util::timer mytimer(evt.c_str(), 5);
    int fdes = UnixOpen(fname, OPEN_READONLY);
    if (fdes < 0) {
	LOGGER(ibis::gVerbose >= 0)
	    << "Warning -- " << evt << " failed to "
	    << "open the named data file, errno = " << errno
	    << strerror(errno);
	return -1;
    }
#if defined(_WIN32) && defined(_MSC_VER)
    (void)_setmode(fdes, _O_BINARY);
#endif

    const uint32_t sz = sizeof(T);
    int ierr = UnixSeek(fdes, 0, SEEK_END);
    if (ierr < 0) {
	LOGGER(ibis::gVerbose >= 0)
	    << "Warning -- " << evt << " failed to seek to the end of file";
	(void) UnixClose(fdes);
	return -2;
    }
    ibis::fileManager::instance().recordPages(0, ierr);
    const uint32_t nrows = ierr / sz;
    ibis::fileManager::buffer<T> buf;
    hits.clear();
    hits.reserve(nrows, u.size()); // reserve space
    ierr = UnixSeek(fdes, 0, SEEK_SET); // point to the beginning of file
    if (buf.size() > 0) { // has a buffer to use
	uint32_t ju = 0;
	uint32_t jv = 0;
	while (ju < u.size() &&
	       (ierr = UnixRead(fdes, buf.address(), buf.size()*sz)) > 0) {
	    for (uint32_t j = 0; ju < u.size() && j < buf.size(); ++ j) {
		while (ju < u.size() && u[ju] < buf[j]) ++ ju;
		if (buf[j] == u[ju]) {
		    hits.setBit(jv+j, 1);
		}
	    }
	    jv += ierr / sz;
	}
    }
    else { // read one value at a time
	T tmp;
	uint32_t ju = 0;
	uint32_t jv = 0;
	while (ju < u.size() &&
	       (ierr = UnixRead(fdes, &tmp, sizeof(tmp))) > 0) {
	    while (ju < u.size() && u[ju] < tmp) ++ ju;
	    if (u[ju] == tmp)
		hits.setBit(jv, 1);
	    ++ jv;
	}
    }
    (void) UnixClose(fdes);
    hits.adjustSize(0, nrows);
    return (ierr > 0 ? 0 : -3);
} // ibis::column::searchSortedOOCD

/// Constructor of index lock.
ibis::column::indexLock::indexLock(const ibis::column* col, const char* m)
    : theColumn(col), mesg(m) {
#if defined(DEBUG) && DEBUG > 0
    ibis::util::logMessage("column::indexLock",
			   "locking column %s for %s", col->name(),
			   (m ? m : "?"));
#endif
    bool toload = false;
    { // only attempt to build the index if idxcnt is zero and idx is zero
	ibis::column::mutexLock(col, m);
	toload = (theColumn->idxcnt() == 0 && theColumn->idx == 0);
    }
    if (toload)
	theColumn->loadIndex();
    if (theColumn->idx != 0) {
	++ theColumn->idxcnt; // increment counter

	int ierr = pthread_rwlock_rdlock(&(col->rwlock));
	if (0 != ierr)
	    col->logWarning("gainReadAccess", "pthread_rwlock_rdlock for "
			    "%s returned %d (%s)", m, ierr, strerror(ierr));
	else if (ibis::gVerbose > 9)
	    col->logMessage("gainReadAccess",
			    "pthread_rwlock_rdlock for %s", m);
    }
}

/// Destructor of index lock.
ibis::column::indexLock::~indexLock() {
#if defined(DEBUG) && DEBUG > 0
    ibis::util::logMessage("column::indexLock",
			   "unlocking column %s (%s)", theColumn->name(),
			   (mesg ? mesg : "?"));
#endif
    if (theColumn->idx != 0) {
	int ierr = pthread_rwlock_unlock(&(theColumn->rwlock));
	if (ierr)
	    theColumn->logWarning("releaseReadAccess",
				  "pthread_rwlock_unlock for %s returned "
				  "%d (%s)", mesg, ierr, strerror(ierr));
	else if (ibis::gVerbose > 9)
	    theColumn->logMessage("releaseReadAccess",
				  "pthread_rwlock_unlock for %s", mesg);

	-- (theColumn->idxcnt); // decrement counter
    }
}

// explicit template instantiation
template long ibis::column::selectValuesT
(const bitvector&, array_t<unsigned char>&, array_t<uint32_t>&) const;
template long ibis::column::selectValuesT
(const bitvector&, array_t<signed char>&, array_t<uint32_t>&) const;
template long ibis::column::selectValuesT
(const bitvector&, array_t<char>&, array_t<uint32_t>&) const;
template long ibis::column::selectValuesT
(const bitvector&, array_t<uint16_t>&, array_t<uint32_t>&) const;
template long ibis::column::selectValuesT
(const bitvector&, array_t<int16_t>&, array_t<uint32_t>&) const;
template long ibis::column::selectValuesT
(const bitvector&, array_t<uint32_t>&, array_t<uint32_t>&) const;
template long ibis::column::selectValuesT
(const bitvector&, array_t<int32_t>&, array_t<uint32_t>&) const;
template long ibis::column::selectValuesT
(const bitvector&, array_t<uint64_t>&, array_t<uint32_t>&) const;
template long ibis::column::selectValuesT
(const bitvector&, array_t<int64_t>&, array_t<uint32_t>&) const;
template long ibis::column::selectValuesT
(const bitvector&, array_t<float>&, array_t<uint32_t>&) const;
template long ibis::column::selectValuesT
(const bitvector&, array_t<double>&, array_t<uint32_t>&) const;

template long ibis::column::castAndWrite
(const array_t<double>& vals, ibis::bitvector& mask, const char special);
template long ibis::column::castAndWrite
(const array_t<double>& vals, ibis::bitvector& mask,
 const unsigned char special);
template long ibis::column::castAndWrite
(const array_t<double>& vals, ibis::bitvector& mask, const int16_t special);
template long ibis::column::castAndWrite
(const array_t<double>& vals, ibis::bitvector& mask, const uint16_t special);
template long ibis::column::castAndWrite
(const array_t<double>& vals, ibis::bitvector& mask, const int32_t special);
template long ibis::column::castAndWrite
(const array_t<double>& vals, ibis::bitvector& mask, const uint32_t special);
template long ibis::column::castAndWrite
(const array_t<double>& vals, ibis::bitvector& mask, const int64_t special);
template long ibis::column::castAndWrite
(const array_t<double>& vals, ibis::bitvector& mask, const uint64_t special);
template long ibis::column::castAndWrite
(const array_t<double>& vals, ibis::bitvector& mask, const float special);
template long ibis::column::castAndWrite
(const array_t<double>& vals, ibis::bitvector& mask, const double special);
