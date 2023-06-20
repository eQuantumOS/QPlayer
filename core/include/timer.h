/*
 * Copyright (c) 2021 Electronics and Telecommunications Research Institute 
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

/**
 * @file    qtimer.h
 * @author  Kisung Jin - ksjin@etri.re.kr
 * @brief       
 */

#ifndef _QTIMER_H_
#define _QTIMER_H_

#include <iostream>
#include <sstream>
#include <string>
#include <time.h>

class QTimer {

public:
	struct timespec startT;
	struct timespec endT;
	double elapsedSec;
	double elapsedMSec;
	double elapsedUSec;
	double elapsedNSec;
	char timeStr[32];

public:
	QTimer(void);
	~QTimer(void);

	void start() {	
		clock_gettime(CLOCK_MONOTONIC, &startT);
	}

	void end() {
		clock_gettime(CLOCK_MONOTONIC, &endT);
		elapsedNSec = ((endT.tv_sec - startT.tv_sec) * 1000000000LL) + (endT.tv_nsec - startT.tv_nsec);

		calculateTime();
	}

	void lapStart() {
		clock_gettime(CLOCK_MONOTONIC, &startT);
	}

	void lapStop() {
		clock_gettime(CLOCK_MONOTONIC, &endT);
		elapsedNSec += ((endT.tv_sec - startT.tv_sec) * 1000000000LL) + (endT.tv_nsec - startT.tv_nsec);
	}

	void lapFinish() {
		lapStop();
		calculateTime();
	}

	void calculateTime() {
		elapsedUSec = elapsedNSec / 1000LL;
		elapsedMSec = elapsedNSec / 1000000LL;
		elapsedSec  = elapsedNSec / 1000000000LL;

		int h = (int)elapsedSec / 3600;
		int m = (int)(((long)elapsedSec / 60) % 60);
		int s = (int)((long)elapsedSec % 60);

		if(s > 0) {
			double remainingMSec = elapsedMSec - ((uint64_t)elapsedSec * 1000LL);
			sprintf(timeStr, "%02d:%02d:%02d + %dms", h, m, s, (uint32_t)remainingMSec);
		} else if(elapsedMSec >= 1) {
			sprintf(timeStr, "%f ms", elapsedMSec);
		} else if(elapsedUSec >= 1 ) {
			sprintf(timeStr, "%f us", elapsedUSec);
		} else if(elapsedNSec > 0) {
			sprintf(timeStr, "%f ns", elapsedNSec);
		} 
	}

	const char *getTime() { return timeStr; }
	double getElapsedSec() { return elapsedSec; }
	double getElapsedMSec() { return elapsedMSec; }
	double getElapsedUSec() { return elapsedUSec; }
	double getElapsedNSec() { return elapsedNSec; }
};

#endif /* _QIMER_H_ */
