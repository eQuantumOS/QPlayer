#ifndef __LATTICE_H__
#define __LATTICE_H__

#include <stdio.h>

#include "qplayer.h"
#include "ds.h"


struct lqubits_info lqs1 = {
	1,			/* number of logical qubits */
	25,			/* number of physical qubits */
	{
		{
			0,			/* LQ identifier */
			X_LEFT,		/* flavor */
			1,          /* row in lattice */
			1,          /* col in lattice */
			25,			/* number of data qubits */
			{
				9,       10,      11,      12,
				     0,       1,       2,
				13,      14,      15,      16,
				     3,       4,       5,
				17,      18,      19,      20,
				     6,       7,       8,
				21,      22,      23,      24
			}
		}
	}
};

struct lqubits_info lqs2 = {
	2,			/* number of logical qubits */
	45,			/* number of physical qubits */
	{
		{
			0,			/* LQ identifier */
			X_LEFT,		/* flavor */
			1,          /* row in lattice */
			1,          /* col in lattice */
			25,			/* number of data qubits */
			{
				18,      19,      20,      21,
				     0,       1,       2,
				25,      26,      27,      28,
				     3,       4,       5,
				32,      33,      34,      35,
				     6,       7,       8,
				36,      37,      38,      39
			}
		},
		{
			1,			/* LQ identifier */
			X_RIGHT,	/* flavor */
			1,          /* row in lattice */
			2,          /* col in lattice */
			25,			/* number of data qubits */
			{
				21,       22,       23,       24,
				      9,       10,       11,
				28,       29,       30,       31,
				     12,       13,       14,
				35,       36,       37,       38,
				     15,       16,       17,
				39,       40,       41,       42
			}
		}
	}
};

struct lqubits_info lqs3 = {
	3,			/* number of logical qubits */
	67,			/* number of physical qubits */
	{
		{
			0,			/* LQ identifier */
			X_RIGHT,		/* flavor */
			1,          /* row in lattice */
			1,          /* col in lattice */
			25,			/* number of data qubits */
			{
				27,      28,      29,      30,
				     0,       1,       2,
				37,      38,      39,      40,
				     3,       4,       5,
				47,      48,      49,      50,
				     6,       7,       8,
				57,      58,      59,      60
			}
		},
		{
			1,			/* LQ identifier */
			X_LEFT,	/* flavor */
			1,          /* row in lattice */
			2,          /* col in lattice */
			25,			/* number of data qubits */
			{
				30,       31,       32,       33,
				      9,       10,       11,
				40,       41,       42,       43,
				     12,       13,       14,
				50,       52,       53,       54,
				     15,       16,       17,
				60,       61,       62,       63
			}
		},
		{
			2,			/* LQ identifier */
			X_LEFT,		/* flavor */
			2,          /* row in lattice */
			1,          /* col in lattice */
			25,			/* number of data qubits */
			{
				33,       34,       35,       36,
				     18,       19,       20,
				43,       44,       45,       46,
				     21,       22,       23,
				53,       54,       55,       56,
				     24,       25,       26,
				63,       64,       65,       66
			}
		}
	}
};

struct lqubits_info lqs4 = {
	4,			/* number of logical qubits */
	85,			/* number of physical qubits */
	{
		{
			0,			/* LQ identifier */
			X_RIGHT,		/* flavor */
			1,          /* row in lattice */
			1,          /* col in lattice */
			25,			/* number of data qubits */
			{
				36,      37,      38,      39,
				     0,       1,       2,
				43,      44,      45,      46,
				     3,       4,       5,
				50,      51,      52,      53,
				     6,       7,       8,
				57,      58,      59,      60
			}
		},
		{
			1,			/* LQ identifier */
			X_LEFT,	/* flavor */
			1,          /* row in lattice */
			2,          /* col in lattice */
			25,			/* number of data qubits */
			{
				39,       40,       41,       42,
				      9,       10,       11,
				46,       47,       48,       49,
				     12,       13,       14,
				53,       54,       55,       56,
				     15,       16,       17,
				60,       61,       62,       63
			}
		},
		{
			2,			/* LQ identifier */
			X_LEFT,		/* flavor */
			2,          /* row in lattice */
			1,          /* col in lattice */
			25,			/* number of data qubits */
			{
				57,       58,       59,       60,
				     18,       19,       20,
				64,       65,       66,       67,
				     21,       22,       23,
				71,       72,       73,       74,
				     24,       25,       26,
				78,       79,       80,       81
			}
		},
		{
			3,			/* LQ identifier */
			X_RIGHT,	/* flavor */
			2,          /* row in lattice */
			1,          /* col in lattice */
			25,			/* number of data qubits */
			{
				60,       62,       63,       64,
				     27,       28,       29,
				67,       68,       69,       70,
				     30,       31,       32,
				74,       75,       76,       77,
				     33,       34,       35,
				81,       82,       83,       84
			}
		}
	}
};

struct lqubits_info lqs5 = {
	5,			/* number of logical qubits */
	105,			/* number of physical qubits */
	{
		{
			0,			/* LQ identifier */
			X_RIGHT,		/* flavor */
			1,          /* row in lattice */
			1,          /* col in lattice */
			25,			/* number of data qubits */
			{
				45,      46,      47,      48,
				     0,       1,       2,
				55,      56,      57,      58,
				     3,       4,       5,
				65,      66,      67,      68,
				     6,       7,       8,
				75,      76,      77,      78
			}
		},
		{
			1,			/* LQ identifier */
			X_LEFT,	/* flavor */
			1,          /* row in lattice */
			2,          /* col in lattice */
			25,			/* number of data qubits */
			{
				48,       49,       50,       51,
				      9,       10,       11,
				58,       59,       60,       61,
				     12,       13,       14,
				68,       69,       70,       71,
				     15,       16,       17,
				78,       79,       80,       81
			}
		},
		{
			2,			/* LQ identifier */
			X_LEFT,		/* flavor */
			2,          /* row in lattice */
			1,          /* col in lattice */
			25,			/* number of data qubits */
			{
				51,       52,       53,       54,
				     18,       19,       20,
				61,       62,       63,       64,
				     21,       22,       23,
				71,       72,       73,       74,
				     24,       25,       26,
				81,       82,       83,       84
			}
		},
		{
			3,			/* LQ identifier */
			X_RIGHT,	/* flavor */
			2,          /* row in lattice */
			1,          /* col in lattice */
			25,			/* number of data qubits */
			{
				75,       76,       77,       78,
				     27,       28,       29,
				85,       86,       87,       88,
				     30,       31,       32,
				92,       93,       94,       95,
				     33,       34,       35,
				99,       100,      101,      102
			}
		},
		{
			4,			/* LQ identifier */
			X_LEFT,		/* flavor */
			2,          /* row in lattice */
			2,          /* col in lattice */
			25,			/* number of data qubits */
			{
				78,       79,       80,       81,
				     36,       37,       38,
				88,       89,       90,       91,
				     39,       40,       41,
				95,       96,       97,       98,
				     42,       43,       44,
				102,     103,      104,      105
			}
		}
	}
};

struct lqubits_info lqs6 = {
	6,			/* number of logical qubits */
	85,			/* number of physical qubits */
	{
		{
			0,			/* LQ identifier */
			X_RIGHT,		/* flavor */
			1,          /* row in lattice */
			1,          /* col in lattice */
			25,			/* number of data qubits */
			{
				36,      37,      38,      39,
				     0,       1,       2,
				43,      44,      45,      46,
				     3,       4,       5,
				50,      51,      52,      53,
				     6,       7,       8,
				57,      58,      59,      60
			}
		},
		{
			1,			/* LQ identifier */
			X_LEFT,	/* flavor */
			1,          /* row in lattice */
			2,          /* col in lattice */
			25,			/* number of data qubits */
			{
				39,       40,       41,       42,
				      9,       10,       11,
				46,       47,       48,       49,
				     12,       13,       14,
				53,       54,       55,       56,
				     15,       16,       17,
				60,       61,       62,       63
			}
		},
		{
			2,			/* LQ identifier */
			X_LEFT,		/* flavor */
			2,          /* row in lattice */
			1,          /* col in lattice */
			25,			/* number of data qubits */
			{
				57,       58,       59,       60,
				     18,       19,       20,
				64,       65,       66,       67,
				     21,       22,       23,
				71,       72,       73,       74,
				     24,       25,       26,
				78,       79,       80,       81
			}
		},
		{
			3,			/* LQ identifier */
			X_RIGHT,	/* flavor */
			2,          /* row in lattice */
			1,          /* col in lattice */
			25,			/* number of data qubits */
			{
				60,       62,       63,       64,
				     27,       28,       29,
				67,       68,       69,       70,
				     30,       31,       32,
				74,       75,       76,       77,
				     33,       34,       35,
				81,       82,       83,       84
			}
		}
	}
};

struct lqubits_info lqs9 = {
	9,			/* number of logical qubits */
	181,		/* number of physical qubits */
	{
		{
			0,			/* LQ identifier */
			X_RIGHT,	/* flavor */
			1,			/* rows in lattice */
			1,			/* cols in lattice */
			25,			/* number of data qubits */
			{
	 			 81,         82,         83,       84,
				       0,          1,          2,
			  	 91,         92,         93,       94,
				       3,          4,          5,
				101,        102,        103,      104,
				       6,          7,          8,
				111,        112,        113,      114
			}
		},
		{
			1,			/* LQ identifier */
			X_LEFT,		/* flavor */
			1,			/* rows in lattice */
			2,			/* cols in lattice */
			25,			/* number of data qubits */
			{
				 84,         85,         86,       87,
				       9,         10,         11,
				 94,         95,         96,       97,
				      12,         13,         14,
				104,        105,        106,      107,
				      15,         16,         17,
				114,        115,        116,      117
			}
		},
		{
			2,			/* LQ identifier */
			X_RIGHT,	/* flavor */
			1,			/* rows in lattice */
			3,			/* cols in lattice */
			25,			/* number of data qubits */
			{
				 87,         88,         89,        90,
				      18,         19,         20,
				 97,         98,         99,       100,
				      21,         22,         23,
				107,        108,        109,       110,
				      24,         25,         26,
				117,        118,        119,       120
			}
		},
		{
			3,			/* LQ identifier */
			X_LEFT,		/* flavor */
			2,			/* rows in lattice */
			1,			/* cols in lattice */
			25,			/* number of data qubits */
			{
				111,        112,        113,      114,
				      27,         28,         29,
				121,        122,        123,      124,
				      30,         31,         32,
				131,        132,        133,      134,
				      33,         34,         35,
				141,        142,        143,      144
			}
		},
		{
			4,			/* LQ identifier */
			X_RIGHT,	/* flavor */
			2,			/* rows in lattice */
			2,			/* cols in lattice */
			25,			/* number of data qubits */
			{
				114,        115,        116,      117,
 				      36,         37,         38,
				124,        125,        126,      127,
				      39,         40,         41,
				134,        135,        136,      137,
				      42,         43,         44,
				144,        145,        146,      147
			}
		},
		{
			5,			/* LQ identifier */
			X_LEFT,		/* flavor */
			2,			/* rows in lattice */
			3,			/* cols in lattice */
			25,			/* number of data qubits */
			{
				117,        118,        119,      120,
				      45,         46,         47,
				127,        128,        129,      130,
				      48,         49,         50,
				137,        138,        139,      140,
				      51,         52,         53,
				147,        148,        149,      150
			}
		},
		{
			6,			/* LQ identifier */
			X_RIGHT,	/* flavor */
			3,			/* rows in lattice */
			1,			/* cols in lattice */
			25,			/* number of data qubits */
			{
				141,        142,        143,      144,
				      54,         55,         56,
				151,        152,        153,      154,
				      57,         58,         59,
				161,        162,        163,      164,
				      60,         61,         62,
				171,        172,        173,      174
			}
		},
		{
			7,			/* LQ identifier */
			X_LEFT,		/* flavor */
			3,			/* rows in lattice */
			2,			/* cols in lattice */
			25,			/* number of data qubits */
			{
				144,        145,        146,      147,
				      63,         64,         65,
				154,        155,        156,      157,
				      66,         67,         68,
				164,        165,        166,      167,
				      69,         70,         71,
				174,        175,        176,      177
			}
		},
		{
			8,			/* LQ identifier */
			X_RIGHT,	/* flavor */
			3,			/* rows in lattice */
			3,			/* cols in lattice */
			25,			/* number of data qubits */
			{
				147,        148,        149,      150,
				      72,         73,         74,
				157,        158,        159,      160,
				      75,         76,         77,
				167,        168,        169,      170,
				      78,         79,         80,
				177,        178,        179,      180
			}
		}
	}
};

struct lqubits_info lqs16 = {
	16,			/* number of logical qubits */
	313,		/* number of physical qubits */
	{
		{
			0,			/* LQ identifier */
			X_LEFT,		/* flavor */
			1,			/* rows in lattice */
			1,			/* cols in lattice */
			25,			/* number of data qubits */
			{
				144,        145,        146,      147,
				       0,          1,          2,
				157,        158,        159,      160,
				       3,          4,          5,
				170,        171,        172,      173,
				       6,          7,          8,
				183,        184,        185,      186
			}
		},
		{
			1,			/* LQ identifier */
			X_RIGHT,	/* flavor */
			1,			/* rows in lattice */
			2,			/* cols in lattice */
			25,			/* number of data qubits */
			{
				147,        148,        149,      150,
				       9,         10,         11,
				160,        161,        162,      163,
				      12,         13,         14,
				173,        174,        175,      176,
				      15,         16,         17,
				186,        187,        188,      189
			}
		},
		{
			2,			/* LQ identifier */
			X_LEFT,		/* flavor */
			1,			/* rows in lattice */
			3,			/* cols in lattice */
			25,			/* number of data qubits */
			{
				150,        151,        152,      153,
				      18,         19,         20,
				163,        164,        165,      166,
				      21,         22,         23,
				176,        177,        178,      179,
				      24,         25,         26,
				189,        190,        191,      192
			}
		},
		{
			3,			/* LQ identifier */
			X_RIGHT,	/* flavor */
			1,			/* rows in lattice */
			4,			/* cols in lattice */
			25,			/* number of data qubits */
			{
				153,        154,        155,      156,
				      27,         28,         29,
				166,        167,        168,      169,
				      30,         31,         32,
				179,        180,        181,      182,
				      33,         34,         35,
				192,        193,        194,      195
			}
		},
		{
			4,			/* LQ identifier */
			X_RIGHT,	/* flavor */
			2,			/* rows in lattice */
			1,			/* cols in lattice */
			25,			/* number of data qubits */
			{
				183,        184,        185,      186,
 				      36,         37,         38,
				196,        197,        198,      199,
				      39,         40,         41,
				209,        210,        211,      212,
				      42,         43,         44,
				222,        223,        224,      225
			}
		},
		{
			5,			/* LQ identifier */
			X_LEFT,		/* flavor */
			2,			/* rows in lattice */
			2,			/* cols in lattice */
			25,			/* number of data qubits */
			{
				186,        187,        188,      189,
				      45,         46,         47,
				199,        200,        201,      202,
				      48,         49,         50,
				212,        213,        214,      215,
				      51,         52,         53,
				225,        226,        227,      228
			}
		},
		{
			6,			/* LQ identifier */
			X_RIGHT,	/* flavor */
			2,			/* rows in lattice */
			3,			/* cols in lattice */
			25,			/* number of data qubits */
			{
				189,        190,        191,      192,
				      54,         55,         56,
				202,        203,        204,      205,
				      57,         58,         59,
				215,        216,        217,      218,
				      60,         61,         62,
				228,        229,        230,      231
			}
		},
		{
			7,			/* LQ identifier */
			X_LEFT,		/* flavor */
			2,			/* rows in lattice */
			4,			/* cols in lattice */
			25,			/* number of data qubits */
			{
				192,        193,        194,      195,
				      63,         64,         65,
				205,        206,        207,      208,
				      66,         67,         68,
				218,        219,        220,      221,
				      69,         70,         71,
				231,        232,        233,      234
			}
		},
		{
			8,			/* LQ identifier */
			X_LEFT,		/* flavor */
			3,			/* rows in lattice */
			1,			/* cols in lattice */
			25,			/* number of data qubits */
			{
				222,        223,        224,      225,
				      72,         73,         74,
				235,        236,        237,      238,
				      75,         76,         77,
				248,        249,        250,      251,
				      78,         79,         80,
				261,        262,        263,      264
			}
		},
		{
			9,			/* LQ identifier */
			X_RIGHT,	/* flavor */
			3,			/* rows in lattice */
			2,			/* cols in lattice */
			25,			/* number of data qubits */
			{
				225,        226,        227,      228,
				      81,         82,         83,
				238,        239,        240,      241,
				      84,         85,         86,
				251,        252,        253,      254,
				      87,         88,         89,
				264,        265,        266,      267
			}
		},
		{
			10,			/* LQ identifier */
			X_LEFT,		/* flavor */
			3,			/* rows in lattice */
			3,			/* cols in lattice */
			25,			/* number of data qubits */
			{
				228,        229,        230,      231,
				      90,         91,         92,
				241,        242,        243,      244,
				      93,         94,         95,
				254,        255,        256,      257,
				      96,         97,         98,
				267,        268,        269,      270
			}
		},
		{
			11,			/* LQ identifier */
			X_RIGHT,	/* flavor */
			3,			/* rows in lattice */
			4,			/* cols in lattice */
			25,			/* number of data qubits */
			{
				231,        232,        233,      234,
				      99,        100,        101,
				244,        245,        246,      247,
				     102,        103,        104,
				257,        258,        259,      260,
				     105,        106,        107,
				270,        271,        272,      273
			}
		},
		{
			12,			/* LQ identifier */
			X_RIGHT,	/* flavor */
			4,			/* rows in lattice */
			1,			/* cols in lattice */
			25,			/* number of data qubits */
			{
				261,        262,        263,      264,
				     108,        109,        110,
				274,        275,        276,      277,
				     111,        112,        113,
				287,        288,        289,      290,
				     114,        115,        116,
				300,        301,        302,      303
			}
		},
		{
			13,			/* LQ identifier */
			X_LEFT,		/* flavor */
			4,			/* rows in lattice */
			2,			/* cols in lattice */
			25,			/* number of data qubits */
			{
				264,        265,        266,      267,
				     117,        118,        119,
				277,        278,        279,      280,
				     120,        121,        122,
				290,        291,        292,      293,
				     123,        124,        125,
				303,        304,        305,      306
			}
		},
		{
			14,			/* LQ identifier */
			X_RIGHT,	/* flavor */
			4,			/* rows in lattice */
			3,			/* cols in lattice */
			25,			/* number of data qubits */
			{
				267,        268,        269,      270,
				     126,        127,        128,
				280,        281,        282,      283,
				     129,        130,        131,
				293,        294,        295,      296,
				     132,        133,        134,
				306,        307,        308,      309
			}
		},
		{
			15,			/* LQ identifier */
			X_LEFT,		/* flavor */
			4,			/* rows in lattice */
			4,			/* cols in lattice */
			25,			/* number of data qubits */
			{
				270,        271,        272,      273,
				     135,        136,        137,
				283,        284,        285,      286,
				     138,        139,        140,
				296,        297,        298,      299,
				     141,        142,        143,
				309,        310,        311,      312
			}
		}
	}
};

struct lqubits_info lqs20 = {
	20,			/* number of logical qubits */
	388,		/* number of physical qubits */
	{
		{
			0,			/* LQ identifier */
			X_RIGHT,	/* flavor */
			1,			/* rows in lattice */
			1,			/* cols in lattice */
			25,			/* number of data qubits */
			{
				180,        181,        182,      183,
				       0,          1,          2,
				196,        197,        198,      199,
				       3,          4,          5,
				212,        213,        214,      215,
				       6,          7,          8,
				228,        229,        230,      231
			}
		},
		{
			1,			/* LQ identifier */
			X_LEFT,		/* flavor */
			1,			/* rows in lattice */
			2,			/* cols in lattice */
			25,			/* number of data qubits */
			{
				183,        184,        185,      186,
				       9,         10,         11,
				199,        200,        201,      202,
				      12,         13,         14,
				215,        216,        217,      218,
				      15,         16,         17,
				231,        232,        233,      234
			}
		},
		{
			2,			/* LQ identifier */
			X_RIGHT,	/* flavor */
			1,			/* rows in lattice */
			3,			/* cols in lattice */
			25,			/* number of data qubits */
			{
				186,        187,        188,      189,
				      18,         19,         20,
				202,        203,        204,      205,
				      21,         22,         23,
				218,        219,        220,      221,
				      24,         25,         26,
				234,        235,        236,      237
			}
		},
		{
			3,			/* LQ identifier */
			X_LEFT,		/* flavor */
			1,			/* rows in lattice */
			4,			/* cols in lattice */
			25,			/* number of data qubits */
			{
				189,        190,        191,      192,
				      27,         28,         29,
				205,        206,        207,      208,
				      30,         31,         32,
				221,        222,        223,      224,
				      33,         34,         35,
				237,        238,        239,      240
			}
		},
		{
			4,			/* LQ identifier */
			X_RIGHT,	/* flavor */
			1,			/* rows in lattice */
			5,			/* cols in lattice */
			25,			/* number of data qubits */
			{
				192,        193,        194,      195,
 				      36,         37,         38,
				208,        209,        210,      211,
				      39,         40,         41,
				224,        225,        226,      227,
				      42,         43,         44,
				240,        241,        242,      243
			}
		},
		{
			5,			/* LQ identifier */
			X_LEFT,		/* flavor */
			2,			/* rows in lattice */
			1,			/* cols in lattice */
			25,			/* number of data qubits */
			{
				228,        229,        230,      231,
				      45,         46,         47,
				244,        245,        246,      247,
				      48,         49,         50,
				260,        261,        262,      263,
				      51,         52,         53,
				276,        277,        278,      279
			}
		},
		{
			6,			/* LQ identifier */
			X_RIGHT,	/* flavor */
			2,			/* rows in lattice */
			2,			/* cols in lattice */
			25,			/* number of data qubits */
			{
				231,        232,        233,      234,
				      54,         55,         56,
				247,        248,        249,      250,
				      57,         58,         59,
				263,        264,        265,      266,
				      60,         61,         62,
				279,        280,        281,      282
			}
		},
		{
			7,			/* LQ identifier */
			X_LEFT,		/* flavor */
			2,			/* rows in lattice */
			3,			/* cols in lattice */
			25,			/* number of data qubits */
			{
				234,        235,        236,      237,
				      63,         64,         65,
				250,        251,        252,      253,
				      66,         67,         68,
				266,        267,        268,      269,
				      69,         70,         71,
				282,        283,        284,      285
			}
		},
		{
			8,			/* LQ identifier */
			X_RIGHT,	/* flavor */
			2,			/* rows in lattice */
			4,			/* cols in lattice */
			25,			/* number of data qubits */
			{
				237,        238,        239,      240,
				      72,         73,         74,
				253,        254,        255,      256,
				      75,         76,         77,
				269,        270,        271,      272,
				      78,         79,         80,
				285,        286,        287,      288
			}
		},
		{
			9,			/* LQ identifier */
			X_LEFT,		/* flavor */
			2,			/* rows in lattice */
			5,			/* cols in lattice */
			25,			/* number of data qubits */
			{
				240,        241,        242,      243,
				      81,         82,         83,
				256,        257,        258,      259,
				      84,         85,         86,
				272,        273,        274,      275,
				      87,         88,         89,
				288,        289,        290,      291
			}
		},
		{
			10,			/* LQ identifier */
			X_RIGHT,	/* flavor */
			3,			/* rows in lattice */
			1,			/* cols in lattice */
			25,			/* number of data qubits */
			{
				276,        277,        278,      279,
				      90,         91,         92,
				292,        293,        294,      295,
				      93,         94,         95,
				308,        309,        310,      311,
				      96,         97,         98,
				324,        325,        326,      327
			}
		},
		{
			11,			/* LQ identifier */
			X_LEFT,		/* flavor */
			3,			/* rows in lattice */
			2,			/* cols in lattice */
			25,			/* number of data qubits */
			{
				279,        280,        281,      282,
				      99,        100,        101,
				295,        296,        297,      298,
				     102,        103,        104,
				311,        312,        313,      314,
				     105,        106,        107,
				327,        328,        329,      330
			}
		},
		{
			12,			/* LQ identifier */
			X_LEFT,		/* flavor */
			3,			/* rows in lattice */
			3,			/* cols in lattice */
			25,			/* number of data qubits */
			{
				282,        283,        284,      285,
				     108,        109,        110,
				298,        299,        300,      301,
				     111,        112,        113,
				314,        315,        316,      317,
				     114,        115,        116,
				330,        331,        332,      333
			}
		},
		{
			13,			/* LQ identifier */
			X_LEFT,		/* flavor */
			3,			/* rows in lattice */
			4,			/* cols in lattice */
			25,			/* number of data qubits */
			{
				285,        286,        287,      288,
				     117,        118,        119,
				301,        302,        303,      304,
				     120,        121,        122,
				317,        318,        319,      320,
				     123,        124,        125,
				333,        334,        335,      336
			}
		},
		{
			14,			/* LQ identifier */
			X_RIGHT,	/* flavor */
			3,			/* rows in lattice */
			5,			/* cols in lattice */
			25,			/* number of data qubits */
			{
				288,        289,        290,      291,
				     126,        127,        128,
				304,        305,        306,      307,
				     129,        130,        131,
				320,        321,        322,      323,
				     132,        133,        134,
				336,        337,        338,      339
			}
		},
		{
			15,			/* LQ identifier */
			X_LEFT,		/* flavor */
			4,			/* rows in lattice */
			1,			/* cols in lattice */
			25,			/* number of data qubits */
			{
				324,        325,        326,      327,
				     135,        136,        137,
				340,        341,        342,      343,
				     138,        139,        140,
				356,        357,        358,      359,
				     141,        142,        143,
				372,        373,        374,      375
			}
		},
		{
			16,			/* LQ identifier */
			X_RIGHT,	/* flavor */
			4,			/* rows in lattice */
			2,			/* cols in lattice */
			25,			/* number of data qubits */
			{
				327,        328,        329,      330,
				     144,        145,        146,
				343,        344,        345,      346,
				     147,        148,        149,
				359,        360,        361,      362,
				     150,        151,        152,
				375,        376,        377,      378
			}
		},
		{
			17,			/* LQ identifier */
			X_LEFT,		/* flavor */
			4,			/* rows in lattice */
			3,			/* cols in lattice */
			25,			/* number of data qubits */
			{
				330,        331,        332,      333,
				     153,        154,        155,
				346,        347,        348,      349,
				     156,        157,        158,
				362,        363,        364,      365,
				     159,        160,        161,
				378,        379,        380,      381
			}
		},
		{
			18,			/* LQ identifier */
			X_LEFT,		/* flavor */
			4,			/* rows in lattice */
			4,			/* cols in lattice */
			25,			/* number of data qubits */
			{
				333,        334,        335,      336,
				     162,        163,        164,
				349,        350,        351,      352,
				     165,        166,        167,
				365,        366,        367,      368,
				     168,        169,        170,
				381,        382,        383,      384
			}
		},
		{
			19,			/* LQ identifier */
			X_LEFT,		/* flavor */
			4,			/* rows in lattice */
			5,			/* cols in lattice */
			25,			/* number of data qubits */
			{
				336,        337,        338,      339,
				     171,        172,        173,
				352,        353,        354,      355,
				     174,        175,        176,
				368,        369,        370,      371,
				     177,        178,        179,
				384,        385,        386,      387
			}
		}
	}
};

struct lqubits_info lqs25 = {
	25,			/* number of logical qubits */
	481,		/* number of physical qubits */
	{
		{
			0,			/* LQ identifier */
			X_RIGHT,	/* flavor */
			1,			/* rows in lattice */
			1,			/* cols in lattice */
			25,			/* number of data qubits */
			{
				225,        226,        227,      228,
				       0,          1,          2,
				241,        242,        243,      244,
				       3,          4,          5,
				257,        258,        259,      260,
				       6,          7,          8,
				273,        274,        275,      276
			}
		},
		{
			1,			/* LQ identifier */
			X_LEFT,		/* flavor */
			1,			/* rows in lattice */
			2,			/* cols in lattice */
			25,			/* number of data qubits */
			{
				228,        229,        230,      231,
				       9,         10,         11,
				244,        245,        246,      247,
				      12,         13,         14,
				260,        261,        262,      263,
				      15,         16,         17,
				276,        277,        278,      279
			}
		},
		{
			2,			/* LQ identifier */
			X_RIGHT,	/* flavor */
			1,			/* rows in lattice */
			3,			/* cols in lattice */
			25,			/* number of data qubits */
			{
				231,        232,        233,      234,
				      18,         19,         20,
				247,        248,        249,      250,
				      21,         22,         23,
				263,        264,        265,      266,
				      24,         25,         26,
				279,        280,        281,      282
			}
		},
		{
			3,			/* LQ identifier */
			X_LEFT,		/* flavor */
			1,			/* rows in lattice */
			4,			/* cols in lattice */
			25,			/* number of data qubits */
			{
				234,        235,        236,      237,
				      27,         28,         29,
				250,        251,        252,      253,
				      30,         31,         32,
				266,        267,        268,      269,
				      33,         34,         35,
				282,        283,        284,      285
			}
		},
		{
			4,			/* LQ identifier */
			X_RIGHT,	/* flavor */
			1,			/* rows in lattice */
			5,			/* cols in lattice */
			25,			/* number of data qubits */
			{
				237,        238,        239,      240,
 				      36,         37,         38,
				253,        254,        255,      256,
				      39,         40,         41,
				269,        270,        271,      272,
				      42,         43,         44,
				285,        286,        287,      288
			}
		},
		{
			5,			/* LQ identifier */
			X_LEFT,		/* flavor */
			2,			/* rows in lattice */
			1,			/* cols in lattice */
			25,			/* number of data qubits */
			{
				273,        274,        275,      276,
				      45,         46,         47,
				289,        290,        291,      292,
				      48,         49,         50,
				305,        306,        307,      308,
				      51,         52,         53,
				321,        322,        323,      324
			}
		},
		{
			6,			/* LQ identifier */
			X_RIGHT,	/* flavor */
			2,			/* rows in lattice */
			2,			/* cols in lattice */
			25,			/* number of data qubits */
			{
				276,        277,        278,      279,
				      54,         55,         56,
				292,        293,        294,      295,
				      57,         58,         59,
				308,        309,        310,      311,
				      60,         61,         62,
				324,        325,        326,      327
			}
		},
		{
			7,			/* LQ identifier */
			X_LEFT,		/* flavor */
			2,			/* rows in lattice */
			3,			/* cols in lattice */
			25,			/* number of data qubits */
			{
				279,        280,        281,      282,
				      63,         64,         65,
				295,        296,        297,      298,
				      66,         67,         68,
				311,        312,        313,      314,
				      69,         70,         71,
				327,        328,        329,      330
			}
		},
		{
			8,			/* LQ identifier */
			X_RIGHT,	/* flavor */
			2,			/* rows in lattice */
			4,			/* cols in lattice */
			25,			/* number of data qubits */
			{
				282,        283,        284,      285,
				      72,         73,         74,
				298,        299,        300,      301,
				      75,         76,         77,
				314,        315,        316,      317,
				      78,         79,         80,
				330,        331,        332,      333
			}
		},
		{
			9,			/* LQ identifier */
			X_LEFT,		/* flavor */
			2,			/* rows in lattice */
			5,			/* cols in lattice */
			25,			/* number of data qubits */
			{
				285,        286,        287,      288,
				      81,         82,         83,
				301,        302,        303,      304,
				      84,         85,         86,
				317,        318,        319,      320,
				      87,         88,         89,
				333,        334,        335,      336
			}
		},
		{
			10,			/* LQ identifier */
			X_RIGHT,	/* flavor */
			3,			/* rows in lattice */
			1,			/* cols in lattice */
			25,			/* number of data qubits */
			{
				321,        322,        323,      324,
				      90,         91,         92,
				337,        338,        339,      340,
				      93,         94,         95,
				353,        354,        355,      356,
				      96,         97,         98,
				369,        370,        371,      372
			}
		},
		{
			11,			/* LQ identifier */
			X_LEFT,		/* flavor */
			3,			/* rows in lattice */
			2,			/* cols in lattice */
			25,			/* number of data qubits */
			{
				324,        325,        326,      327,
				      99,        100,        101,
				340,        341,        342,      343,
				     102,        103,        104,
				356,        357,        358,      359,
				     105,        106,        107,
				372,        373,        374,      375
			}
		},
		{
			12,			/* LQ identifier */
			X_LEFT,		/* flavor */
			3,			/* rows in lattice */
			3,			/* cols in lattice */
			25,			/* number of data qubits */
			{
				327,        328,        329,      330,
				     108,        109,        110,
				343,        344,        345,      346,
				     111,        112,        113,
				359,        360,        361,      362,
				     114,        115,        116,
				375,        376,        377,      378
			}
		},
		{
			13,			/* LQ identifier */
			X_LEFT,		/* flavor */
			3,			/* rows in lattice */
			4,			/* cols in lattice */
			25,			/* number of data qubits */
			{
				330,        331,        332,      333,
				     117,        118,        119,
				346,        347,        348,      349,
				     120,        121,        122,
				362,        363,        364,      365,
				     123,        124,        125,
				378,        379,        380,      381
			}
		},
		{
			14,			/* LQ identifier */
			X_RIGHT,	/* flavor */
			3,			/* rows in lattice */
			5,			/* cols in lattice */
			25,			/* number of data qubits */
			{
				333,        334,        335,      336,
				     126,        127,        128,
				349,        350,        351,      352,
				     129,        130,        131,
				365,        366,        367,      368,
				     132,        133,        134,
				381,        382,        383,      384
			}
		},
		{
			15,			/* LQ identifier */
			X_LEFT,		/* flavor */
			4,			/* rows in lattice */
			1,			/* cols in lattice */
			25,			/* number of data qubits */
			{
				369,        370,        371,      372,
				     135,        136,        137,
				385,        386,        387,      388,
				     138,        139,        140,
				401,        402,        403,      404,
				     141,        142,        143,
				417,        418,        419,      420
			}
		},
		{
			16,			/* LQ identifier */
			X_RIGHT,	/* flavor */
			4,			/* rows in lattice */
			2,			/* cols in lattice */
			25,			/* number of data qubits */
			{
				372,        373,        374,      375,
				     144,        145,        146,
				388,        389,        390,      391,
				     147,        148,        149,
				404,        405,        406,      407,
				     150,        151,        152,
				420,        421,        422,      423
			}
		},
		{
			17,			/* LQ identifier */
			X_LEFT,		/* flavor */
			4,			/* rows in lattice */
			3,			/* cols in lattice */
			25,			/* number of data qubits */
			{
				375,        376,        377,      378,
				     153,        154,        155,
				391,        392,        393,      394,
				     156,        157,        158,
				407,        408,        409,      410,
				     159,        160,        161,
				423,        424,        425,      426
			}
		},
		{
			18,			/* LQ identifier */
			X_LEFT,		/* flavor */
			4,			/* rows in lattice */
			4,			/* cols in lattice */
			25,			/* number of data qubits */
			{
				378,        379,        380,      381,
				     162,        163,        164,
				394,        395,        396,      397,
				     165,        166,        167,
				410,        411,        412,      413,
				     168,        169,        170,
				426,        427,        428,      429
			}
		},
		{
			19,			/* LQ identifier */
			X_LEFT,		/* flavor */
			4,			/* rows in lattice */
			5,			/* cols in lattice */
			25,			/* number of data qubits */
			{
				381,        382,        383,      384,
				     171,        172,        173,
				397,        398,        399,      400,
				     174,        175,        176,
				413,        414,        415,      416,
				     177,        178,        179,
				429,        430,        431,      432
			}
		},
		{
			20,			/* LQ identifier */
			X_RIGHT,	/* flavor */
			5,			/* rows in lattice */
			1,			/* cols in lattice */
			25,			/* number of data qubits */
			{
				417,        418,        419,      420,
				     180,        181,        182,
				433,        434,        435,      436,
				     183,        184,        185,
				449,        450,        451,      452,
				     186,        187,        188,
				465,        466,        467,      468
			}
		},
		{
			21,			/* LQ identifier */
			X_LEFT,		/* flavor */
			5,			/* rows in lattice */
			2,			/* cols in lattice */
			25,			/* number of data qubits */
			{
				420,        421,        422,      423,
				     189,        190,        191,
				436,        437,        438,      439,
				     192,        193,        194,
				452,        453,        454,      455,
				     195,        196,        197,
				468,        469,        470,      471
			}
		},
		{
			22,			/* LQ identifier */
			X_RIGHT,	/* flavor */
			5,			/* rows in lattice */
			3,			/* cols in lattice */
			25,			/* number of data qubits */
			{
				423,        424,        425,      426,
				     198,        199,        200,
				439,        440,        441,      442,
				     201,        202,        203,
				455,        456,        457,      458,
				     204,        205,        206,
				471,        472,        473,      474
			}
		},
		{
			23,			/* LQ identifier */
			X_LEFT,		/* flavor */
			5,			/* rows in lattice */
			4,			/* cols in lattice */
			25,			/* number of data qubits */
			{
				426,        427,        428,      429,
				     207,        208,        209,
				442,        443,        444,      445,
				     210,        211,        212,
				458,        459,        460,      461,
				     213,        214,        215,
				474,        475,        476,      477
			}
		},
		{
			24,			/* LQ identifier */
			X_RIGHT,	/* flavor */
			5,			/* rows in lattice */
			5,			/* cols in lattice */
			25,			/* number of data qubits */
			{
				429,        430,        431,      432,
				     216,        217,        218,
				445,        446,        447,      448,
				     219,        220,        221,
				461,        462,        463,      464,
				     222,        223,        224,
				477,        478,        479,      480
			}
		}
	}
};

#endif
