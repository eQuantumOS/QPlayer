#include <stdio.h>
#include <string.h>

#include "qplayer.h"

static int x_syndrome_decoder[2][16][2] = 
{
	//Flavor #0
	{   { -1, -1},  //0000   
        {  6, -1},  //0001
        {  8, -1},  //0010
        {  7, -1},  //0011
        {  0, -1},  //0100
        {  4,  7},  //0101
        {  4, -1},  //0110
        {  4,  6},  //0111
        {  2, -1},  //1000
        {  2,  6},  //1001
        {  1,  4},  //1010
        {  2,  7},  //1011
        {  1, -1},  //1100
        {  1,  6},  //1101
        {  1,  8},  //1110
        {  1,  7}   //1111
	},
	//Flavor #1
	{   { -1, -1},  //0000   
        {  8, -1},  //0001
        {  6, -1},  //0010
        {  7, -1},  //0011
        {  2, -1},  //0100
        {  2,  8},  //0101
        {  4, -1},  //0110
        {  4,  8},  //0111
        {  0, -1},  //1000
        {  0,  8},  //1001
        {  1,  4},  //1010
        {  0,  7},  //1011
        {  1, -1},  //1100
        {  1,  8},  //1101
        {  1,  6},  //1110
        {  1,  7}   //1111
	}
};

static int z_syndrome_decoder[2][16][2] = 
{
	//Flavor #0
	{   { -1, -1},  //0000
        {  8, -1},  //0001
        {  6, -1},  //0010
        {  4,  5},  //0011
        {  1, -1},  //0100
        {  5, -1},  //0101
        {  4, -1},  //0110
        {  4,  8},  //0111
        {  0, -1},  //1000
        {  0,  8},  //1001
        {  3, -1},  //1010
        {  3,  8},  //1011
        {  3,  4},  //1100
        {  0,  5},  //1101
        {  2,  3},  //1110
        {  3,  5}   //1111
	},
	//Flavor #1
	{   { -1, -1},  //0000
        {  2, -1},  //0001
        {  8, -1},  //0010
        {  5, -1},  //0011
        {  0, -1},  //0100
        {  4,  5},  //0101
        {  4, -1},  //0110
        {  2,  4},  //0111
        {  6, -1},  //1000
        {  2,  6},  //1001
        {  3,  4},  //1010
        {  5,  6},  //1011
        {  3, -1},  //1100
        {  2,  3},  //1101
        {  3,  7},  //1110
        {  3,  5}   //1111
	}
};


#define NUM_QUBIT 28
#define STATEVECTOR_LENGTH 30

struct logical_qubit 
{
	int flavor;          // flavor of logical qubit
	int num_total_qubit; // number of total qubit
	int num_data_qubit;  // number of data qubit
	int num_x_stblz;     // number of X stabilizer
	int num_z_stblz;     // number of Z stabilizer
	int data_qubit_list[18];  // order : data qubit, X stabilizer, Z stabilizer
	int x_stblz_list[10];  // X stabilizer qubit list
	int z_stblz_list[10];  // Z stabilizer qubit list
	int x_stblz_scan[10][4]; // CNOT operation sequence for X stabilizer generation
	int z_stblz_scan[10][4]; // CNOT operation sequence for Z stabilizer generation
	int x_stblz_out[10];  // measurement based on Z basis
	int z_stblz_out[10];  // measurement based on Z basis
	int x_operator[3];
	int z_operator[3];
};

static struct logical_qubit lq[16]=
{ 
//#0, flavor 1
  {1, 13, 9, 4, 4, 
   {0, 1, 2, 3, 4, 5, 6, 7, 8}, // qubit_list
   {18, 19, 20, 21},            // x_stblz_list
   {18, 19, 20, 21},            // z_stblz_list
   {{0, 1, -1}, {1, 2, 4, 5}, {3, 4, 6, 7}, {7, 8, -1}}, // x_stblz_scan
   {{3, 6, -1}, {0, 1, 3, 4}, {4, 5, 7, 8}, {2, 5, -1}}, // z_stblz_scan 
														// -1 : end of scan sequence
   {0, 0, 0, 0, 0, 0, 0, 0, 0, 0},  // x_stblz_out
   {0, 0, 0, 0, 0, 0, 0, 0, 0, 0}   // z_stblz_out
  },

//#1, flavor 0
  {0, 13, 9, 4, 4, 
   {9, 10, 11, 12, 13, 14, 15, 16, 17}, // qubit_list
   {22, 23, 24, 25},            // x_stblz_list
   {22, 23, 24, 25},            // z_stblz_list
   {{10, 11, -1}, { 9, 10, 12, 13}, {13, 14, 16, 17}, {15, 16, -1}}, // x_stblz_scan 
   {{ 9, 12, -1}, {10, 11, 13, 14}, {12, 13, 15, 16}, {14, 17, -1}}, // z_stblz_scan 
   {0, 0, 0, 0, 0, 0, 0, 0, 0, 0},  // x_stblz_out
   {0, 0, 0, 0, 0, 0, 0, 0, 0, 0}   // z_stblz_mx
  },

//#2 : Z merge, flavor 2
  {2, 28, 18, 10, 7,
   {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17}, // qubit_list

   //{18, 19, 20, 21, 22, 23, 24, 25, 26, 27}, // x_stblz_list
   {26, 27, 18, 19, 20, 21, 22, 23, 24, 25}, // x_stblz_list

   {18, 19, 20, 21, 23, 24, 25},             // z_stblz_list

//   {{0,   1, -1}, {1, 2, 4, 5},    {3, 4, 6, 7},     {7, 8, -1}, // x_stblz_scan
 //   {10, 11, -1}, {9, 10, 12, 13}, {13, 14, 16, 17}, {15, 16, -1}, 
//	{2, 9, -1}, {5, 12, 8, 15}},
   {{2, 9, -1}, {5, 12, 8, 15},
    {0,   1, -1}, {1, 2, 4, 5},    {3, 4, 6, 7},     {7, 8, -1}, // x_stblz_scan
    {10, 11, -1}, {9, 10, 12, 13}, {13, 14, 16, 17}, {15, 16, -1}}, 

   {{3, 6, -1}, {0, 1, 3, 4}, {4, 5, 7, 8}, {2, 9, 5, 12}, // z_stblz_scan 
    {10, 11, 13, 14}, {12, 13, 15, 16}, {14, 17, -1}}, 
   {0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
   {0, 0, 0, 0, 0, 0, 0, 0, 0, 0}
  },

//#3 : X merge, flavor 3
  {3, 28, 18, 7, 10,
   {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17}, // qubit_list
   {18, 19, 20, 21, 23, 24, 25},             // x_stblz_list
   {26, 27, 18, 19, 20, 21, 22, 23, 24, 25}, // z_stblz_list


   {{0,   1, -1},    {1, 2, 4, 5},     {3, 4, 6, 7}, {7, 8, 10, 11}, // x_stblz_scan
    {9, 10, 12, 13}, {13, 14, 16, 17}, {15, 16, -1}}, 

   {{6, 7, 9, 10}, {8, 11, -1}, 
	{3, 6, -1}, {0, 1, 3, 4}, {4, 5, 7, 8}, {2, 5, -1}, // z_stblz_scan 
	{9, 12, -1},{10, 11, 13, 14}, {12, 13, 15, 16}, {14, 17, -1}}, 

   {0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
   {0, 0, 0, 0, 0, 0, 0, 0, 0, 0}
  },

//#4 : hadamard rotate, flavor 4
  {4, 28, 18, 10, 7,
   {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17}, // qubit_list

   {26, 27, 18, 19, 20, 21, 22, 23, 24, 25}, // x_stblz_list
   {18, 19, 20, 21, 23, 24, 25},             // z_stblz_list

   {{2, 9, -1}, {5, 12, 8, 15},
    {0,   1, -1}, {1, 2, 4, 5},    {3, 4, 6, 7},     {7, 8, -1}, // x_stblz_scan
    {10, 11, -1}, {9, 10, 12, 13}, {13, 14, 16, 17}, {15, 16, -1}}, 

   {{3, 6, -1}, {0, 1, 3, 4}, {4, 5, 7, 8}, {2, 9, 5, 12}, // z_stblz_scan 
    {10, 11, 13, 14}, {12, 13, 15, 16}, {14, 17, -1}}, 
   {0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
   {0, 0, 0, 0, 0, 0, 0, 0, 0, 0}
  },

  { }, //#5
  { }, //#6
  { }, //#7
  { }, //#8
  { }, //#9

//------------------------------------------------------------
// for CNOT

//ancilla
//#10, flavor 1
  {1, 13, 9, 4, 4, 
   {0, 1, 2, 3, 4, 5, 6, 7, 8}, // qubit_list
   {27, 27, 27, 27},            // x_stblz_list
   {27, 27, 27, 27},            // z_stblz_list
   {{0, 1, -1}, {1, 2, 4, 5}, {3, 4, 6, 7}, {7, 8, -1}}, // x_stblz_scan
   {{3, 6, -1}, {0, 1, 3, 4}, {4, 5, 7, 8}, {2, 5, -1}}, // z_stblz_scan 
														// -1 : end of scan sequence
   {0, 0, 0, 0, 0, 0, 0, 0, 0, 0},  // x_stblz_out
   {0, 0, 0, 0, 0, 0, 0, 0, 0, 0},  // z_stblz_out
   {1, 4, 7},
   {3, 4, 5},
  },

//#11, flavor 0
  {0, 13, 9, 4, 4, 
   {9, 10, 11, 12, 13, 14, 15, 16, 17}, // qubit_list
   {27, 27, 27, 27},            // x_stblz_list
   {27, 27, 27, 27},            // z_stblz_list
   {{10, 11, -1}, { 9, 10, 12, 13}, {13, 14, 16, 17}, {15, 16, -1}}, // x_stblz_scan 
   {{ 9, 12, -1}, {10, 11, 13, 14}, {12, 13, 15, 16}, {14, 17, -1}}, // z_stblz_scan 
   {0, 0, 0, 0, 0, 0, 0, 0, 0, 0},  // x_stblz_out
   {0, 0, 0, 0, 0, 0, 0, 0, 0, 0},  // z_stblz_mx
   {10, 13, 16},
   {12, 13, 14},
  },

//#12, flavor 0 (Control)
  {0, 13, 9, 4, 4, 
   {18, 19, 20, 21, 22, 23, 24, 25, 26}, // qubit_list
   {27, 27, 27, 27},            // x_stblz_list
   {27, 27, 27, 27},            // z_stblz_list
   {{19, 20, -1}, {18, 19, 21, 22}, {22, 23, 25, 26}, {24, 25, -1}}, // x_stblz_scan 
   {{18, 21, -1}, {19, 20, 22, 23}, {21, 22, 24, 25}, {23, 26, -1}}, // z_stblz_scan 
   {0, 0, 0, 0, 0, 0, 0, 0, 0, 0},  // x_stblz_out
   {0, 0, 0, 0, 0, 0, 0, 0, 0, 0},  // z_stblz_mx
   {19, 22, 25},
   {21, 22, 23},
  },

//#13 : Z merge, flavor 2
  {2, 28, 18, 10, 7,
   {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17}, // qubit_list

   {27, 27, 27, 27, 27, 27, 27, 27, 27, 27}, // x_stblz_list
   {27, 27, 27, 27, 27, 27, 27},             // z_stblz_list

   {{2, 9, -1}, {5, 12, 8, 15},
    {0,   1, -1}, {1, 2, 4, 5},    {3, 4, 6, 7},     {7, 8, -1}, // x_stblz_scan
    {10, 11, -1}, {9, 10, 12, 13}, {13, 14, 16, 17}, {15, 16, -1}}, 

   {{3, 6, -1}, {0, 1, 3, 4}, {4, 5, 7, 8}, {2, 9, 5, 12}, // z_stblz_scan 
    {10, 11, 13, 14}, {12, 13, 15, 16}, {14, 17, -1}}, 
   {0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
   {0, 0, 0, 0, 0, 0, 0, 0, 0, 0}
  },

//#14 : X merge, flavor 3
  {3, 28, 18, 7, 10,
   {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17}, // qubit_list
   {27, 27, 27, 27, 27, 27, 27},             // x_stblz_list
   {27, 27, 27, 27, 27, 27, 27, 27, 27, 27}, // z_stblz_list


   {{0,   1, -1},    {1, 2, 4, 5},     {3, 4, 6, 7}, {7, 8, 19, 20}, // x_stblz_scan
    {18, 19, 21, 22}, {22, 23, 25, 26}, {24, 25, -1}}, 

   {{6, 7, 18, 19}, {8, 20, -1}, 
	{3, 6, -1}, {0, 1, 3, 4}, {4, 5, 7, 8}, {2, 5, -1}, // z_stblz_scan 
	{18, 21, -1},{19, 20, 22, 23}, {21, 22, 24, 25}, {23, 26, -1}}, 

   {0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
   {0, 0, 0, 0, 0, 0, 0, 0, 0, 0}
  },



//#15 : X merge, flavor 3
  {3, 28, 18, 7, 10,
   {9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26}, // qubit_list
   {27, 27, 27, 27, 27, 27, 27},             // x_stblz_list
   {27, 27, 27, 27, 27, 27, 27, 27, 27, 27}, // z_stblz_list


   {{0,   1, -1},    {1, 2, 4, 5},     {3, 4, 6, 7}, {7, 8, 19, 20}, // x_stblz_scan
    {18, 19, 21, 22}, {22, 23, 25, 26}, {24, 25, -1}}, 

   {{6, 7, 18, 19}, {8, 20, -1}, 
	{3, 6, -1}, {0, 1, 3, 4}, {4, 5, 7, 8}, {2, 5, -1}, // z_stblz_scan 
	{18, 21, -1},{19, 20, 22, 23}, {21, 22, 24, 25}, {23, 26, -1}}, 

   {0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
   {0, 0, 0, 0, 0, 0, 0, 0, 0, 0}
  }



};

//===========================================================
//
//  Logical X operator
//
//===========================================================
extern void logical_x_operator(QRegister *QReg, int lq_no)
{
	int i;
	struct logical_qubit *c_lq;

	c_lq = &(lq[lq_no]);

	printf("-- Apply Logical X operator #%d\n", lq_no);

	for(i=0; i<3; i++)
	{
		X(QReg, c_lq->x_operator[i]);
	}
	
}


//===========================================================
//
//  Logical Z operator
//
//===========================================================
extern void logical_z_operator(QRegister *QReg, int lq_no)
{
	int i;
	struct logical_qubit *c_lq;

	c_lq = &(lq[lq_no]);

	printf("-- Apply Logical Z operator #%d\n", lq_no);

	for(i=0; i<3; i++)
	{
		Z(QReg, c_lq->z_operator[i]);
	}
	
}


//===========================================================
//
// measure logical qubit
//
//===========================================================
extern int measure_lq(QRegister *QReg, int lq_no)
{
	int i, num_data_qubit;
	int measure_data[18];
	struct logical_qubit *c_lq;
	int logical_qubit_eigenvalue;

	c_lq = &(lq[lq_no]);
	num_data_qubit = c_lq->num_data_qubit;


	printf("--------------------------------------------------\n");
	printf("Measure logical qubit (Z basis)\n");
	printf("--------------------------------------------------\n");


	for(i=0; i<num_data_qubit; i++)
		measure_data[i] = M(QReg, c_lq->data_qubit_list[i]);

    printf("Measure Data Qubits  : |");

    for(i=0; i<num_data_qubit; i++) {
		if(i%3 == 0) printf(" ");
		printf("%d", measure_data[i]); 
	}	
	printf(">\n");

	printf("Logical qubit : ");
	logical_qubit_eigenvalue = 1;	
	for(i=0; i<num_data_qubit; i++)
		if(measure_data[i] == 1) // eigenvalue = -1
			logical_qubit_eigenvalue *= -1;
	
	if(logical_qubit_eigenvalue == 1) printf("|0>\n");
	else if(logical_qubit_eigenvalue == -1) printf("|1>\n");
	printf("--------------------------------------------------\n");

	return logical_qubit_eigenvalue;
}

//===========================================================
//
//
// 1) Parameters
//  - dsv_buf :
//  - eval_buf : 
//
//===========================================================
extern void eval_dsv(char *dsv_buf, char *eval_buf)
{
	int i, eval_buf_inx, eval_dsv, dsv_buf_len;

	eval_buf_inx=0;	
	eval_dsv=0;
	dsv_buf_len = strlen(dsv_buf);	



//	printf("################\n");
//	printf("dvs_buf %s\n", dsv_buf);
//	printf("################\n");


	for(i=0; i<dsv_buf_len; i++)
	{
		eval_dsv += dsv_buf[i] - '0';

//		printf("%c %d\n", dsv_buf[i], eval_dsv);

		if((i+1)%9 == 0)
		{
			if(eval_dsv % 2 == 0)
				eval_buf[eval_buf_inx++] = '0';
			else 
				eval_buf[eval_buf_inx++] = '1';

			eval_dsv = 0;
		}
	}
	eval_buf[eval_buf_inx] = '\0';
}



//===========================================================
//
// Reverse src string and store to dst string
// 1) Parameters
//  - src : source string
//  - dst : destination string
//
//===========================================================
extern void reverse_string(char *src, char *dst)
{
	int len, i;

	len=strlen(src);
	dst[len]='\0';

	for(i=0; i<len; i++) 
	{
		dst[len-1-i]=src[i];
	}
}


//===========================================================
//
// 
//
//===========================================================
extern void print_statevector_data_qubit(char *sv, struct logical_qubit *c_lq)
{
	int *data_qubit_list, num_data_qubit;
	int i, inx;
	char rsv[STATEVECTOR_LENGTH];

	num_data_qubit= c_lq->num_data_qubit;
	data_qubit_list = c_lq->data_qubit_list;
	
	reverse_string(sv, rsv);

	/*inx = 1;
	for(i=num_data_qubit-1; i>=0; i--) {
		printf("%c", rsv[data_qubit_list[i]]);
		if((inx)%3 == 0) printf(" ");	
		inx++;
	} */

	for(i=0; i<num_data_qubit; i++) {
		printf("%c", rsv[data_qubit_list[i]]);
		if((i+1)%3 == 0) printf(" ");	
		inx++;
	}




}

//===========================================================
//
// Sort dsv_buff, creal_buf, cimg_buf by dsv_buff in ascending order
//
//===========================================================
extern void bubble_sort(char dsv_buf[][STATEVECTOR_LENGTH], double creal_buf[], 
					double cimg_buf[], int num)
{
	int i, j;
	char tmp_buf[STATEVECTOR_LENGTH];
	double tmp;

	for(i=0; i<num-1; i++) {
		for(j=0; j<num-1; j++)
		{
			if(strcmp(dsv_buf[j], dsv_buf[j+1]) > 0)
			{
				strcpy(tmp_buf, dsv_buf[j]);	
				strcpy(dsv_buf[j], dsv_buf[j+1]);	
				strcpy(dsv_buf[j+1], tmp_buf);	

				tmp = creal_buf[j];
				creal_buf[j] = creal_buf[j+1];
				creal_buf[j+1] = tmp;		

				tmp = cimg_buf[j];
				cimg_buf[j] = cimg_buf[j+1];
				cimg_buf[j+1] = tmp;		
			} 
		}
	}
}

//===========================================================
//
//
//
//===========================================================
extern int count_num_one(char *sv)
{
	int i;
	int num_one=0;


	for(i=0; i<strlen(sv); i++)
	{
		if(sv[i] == '1') 
			num_one++;
	}
	return num_one;
}


//===========================================================
//
//
//
//===========================================================
extern void print_data_qubit(char *sv)
{
	int i;

	for(i=0; i<strlen(sv); i++)
	{
		printf("%c", sv[i]);
		if((i+1)%3 == 0) printf(" ");
	}
}


//===========================================================
//
// Extract data qubit from statevector string
//  1) statevector string order 
//    - sv  (all qubit statevector) : from higher to lower qubit
//    - dsv (data qubit statevector) : from lower data to higher data qubit
//
//===========================================================
extern void extract_data_qubit_from_statevector(char *sv, char *dsv, struct logical_qubit *c_lq)
{
	int *data_qubit_list, num_data_qubit, i;
	char rsv[STATEVECTOR_LENGTH];


	num_data_qubit= c_lq->num_data_qubit;
	data_qubit_list = c_lq->data_qubit_list;

	reverse_string(sv, rsv);

	dsv[num_data_qubit]='\0';

	for(i=0; i<num_data_qubit; i++)
	{
		dsv[i] = rsv[data_qubit_list[i]];
	}


}

//===========================================================
//
// print statevector of logical qubit
//  kind == 0 : print all data
//  kind == 1 : print unique data (delete duplicated data)
//===========================================================
extern void print_statevector_lq(QRegister *QReg, int lq_no, int kind)
{
	FILE *svfile;
	char sv[STATEVECTOR_LENGTH], dsv[STATEVECTOR_LENGTH];
	char  dummy1[10], dummy2[10];
	double creal, cimg;

	char dsv_buf[8192][STATEVECTOR_LENGTH];
	double creal_buf[8192], cimg_buf[8192];
	int dsv_buf_inx, sv_found;
	int i=0, j, num_sv=0;

	struct logical_qubit *c_lq;

	char eval_buf[10];
	char eval_dsv_buf[8192][10];
	int num_dsv=0;
	int eval_dsv_buf_inx, eval_dsv_found;

	int num_even_one=0, num_odd_one=0;


	c_lq = &(lq[lq_no]);

#ifdef QuEST
	reportStateValid(QReg);
#endif
	svfile = fopen("state_rank_0.csv", "rt");
	if(svfile == NULL) 
	{
		printf("Error in open state_rank_0.csv\n");
		return;
	}

	fscanf(svfile, "%s %s\n", dummy1, dummy2);
	printf("---------------------------------------------\n");
	//printf("State vector, logical qubit #%d qubits (index from higher to lower)\n", lq_no);
	printf("State vector, logical qubit #%d qubits (index from lower to higher)\n", lq_no);
	printf("---------------------------------------------\n");

	dsv_buf_inx = 0;
	while(1)
	{
		if(fscanf(svfile, "%s %lf, %lf\n", sv, &creal, &cimg) != 3) break;
		if(creal != 0 || cimg != 0) 
		{
			//--------------------------------
			// find duplicate item in dsv_buf
			//--------------------------------
			extract_data_qubit_from_statevector(sv, dsv, c_lq);
			//printf("Read sv : %s --> dsv : %s\n", sv, dsv);

			sv_found = 0;
			if(kind == 1)
			{
				for(i=0; i<dsv_buf_inx; i++)
				{
					if(strcmp(dsv_buf[i], dsv) == 0) 
					{
						sv_found = 1;
						break;
					}
				}
			}

			//--------------------------------
			// add new dsv to dsv_buf
			//--------------------------------
			if(sv_found == 0) 
			{
				strcpy(dsv_buf[dsv_buf_inx], dsv);
				creal_buf[dsv_buf_inx] = creal;
				cimg_buf[dsv_buf_inx] = cimg;
				dsv_buf_inx++;
			}
			num_sv++;
		} 
	}

	bubble_sort(dsv_buf, creal_buf, cimg_buf, dsv_buf_inx);

	//--------------------------------
	// print
	//--------------------------------
	for(i=0; i<dsv_buf_inx; i++)
	{
		print_data_qubit(dsv_buf[i]);	
		//print_statevector_data_qubit(dsv_buf[i], c_lq);
		//printf(" %lf, %lf\n", creal_buf[i], cimg_buf[i]);
		printf(", %lf, %lf, ", creal_buf[i], cimg_buf[i]);


		if(count_num_one(dsv_buf[i])%2 == 0) 
		{
			num_even_one++;
			printf("[E] ");
			//printf("\n");
		}
		else
		{
			num_odd_one++;
			printf("[O] ");
			//printf("\n");
		}

		eval_dsv(dsv_buf[i], eval_buf);
		printf("|%s> \n", eval_buf);

	}
	printf("Valid state vector [%d], unique vector[%d]\n", num_sv, dsv_buf_inx);
	printf("Even 1 [%d], Odd 1 [%d]\n", num_even_one, num_odd_one);


	eval_dsv_buf_inx = 0;
	num_dsv = 0;
	for(i=0; i<dsv_buf_inx; i++)
	{
		//print_data_qubit(dsv_buf[i]);	
		eval_dsv(dsv_buf[i], eval_buf);
		//printf("%s \n", eval_buf);

		eval_dsv_found = 0;
		if(kind == 1)
		{
			for(j=0; j<eval_dsv_buf_inx; j++)
			{
				if(strcmp(eval_dsv_buf[j], eval_buf) == 0) 
				{
					eval_dsv_found = 1;
					break;
				}
			}
		}

		//--------------------------------
		// add new eval_dsv to eval_dsv_buf
		//--------------------------------
		if(eval_dsv_found == 0) 
		{
			strcpy(eval_dsv_buf[eval_dsv_buf_inx], eval_buf);
			eval_dsv_buf_inx++;
		}
		num_dsv++;
	}

	for(i=0; i<eval_dsv_buf_inx; i++)
	{
		printf("|%s>\n", eval_dsv_buf[i]);

	}
	printf("---------------------------------------------\n");


	fclose(svfile);
}	





//===========================================================
//
//  generate X stabilizer
//
//===========================================================
extern void x_stblz_gen_measure(QRegister *QReg, int lq_no)
{
	int i, j;	
	int num_x_stblz;
	struct logical_qubit *c_lq;

	//printf("--------------------------------------------------\n");
	//printf("  Generate X stabilizer\n");
	printf(" X stabilizer : ");
	//printf("--------------------------------------------------\n");

	c_lq = &(lq[lq_no]);   // current logical qubit
	num_x_stblz =c_lq->num_x_stblz;

    //==================================================
	// Show X stabilizer init value
    //==================================================
	//printf("X stabilizer init value ");
	printf("|");
	for(i=0; i<num_x_stblz; i++) 
		printf("%d", M(QReg, c_lq->x_stblz_list[i]));
	//printf(">\n");
	printf(">");

    //==================================================
	// Generate X stabilizer
    //==================================================
	//printf("Generate X stabilizer\n");
	for(i=0; i<num_x_stblz; i++) {
		H(QReg, c_lq->x_stblz_list[i]);
		//printf("---------\n  hadamard %d\n", c_lq->x_stblz_list[i]);
		for(j=0; j<4; j++) { // max 4 weight
			if(c_lq->x_stblz_scan[i][j] == -1) break; // 2 weight
			CX(QReg, c_lq->x_stblz_list[i],
					c_lq->x_stblz_scan[i][j]);
			//printf("  CNOT %d %d\n", c_lq->x_stblz_list[i],
			//		c_lq->x_stblz_scan[i][j]);
		}
		H(QReg, c_lq->x_stblz_list[i]);
		c_lq->x_stblz_out[i] = M(QReg, c_lq->x_stblz_list[i]); // CNOT
		if(c_lq->x_stblz_out[i] == 1) {  // CNOT
			X(QReg, c_lq->x_stblz_list[i]); //CNOT
		} // CNOT
		//printf("  hadamadrd %d\n", c_lq->x_stblz_list[i]);
	}

	// store x stabilizer measurement result
	//for(i=0; i<num_x_stblz; i++) { // CNOT
	//	c_lq->x_stblz_out[i] = M(QReg, c_lq->x_stblz_list[i]); // CNOT
	//} // CNOT

	//printf("X stabilizer result |");
	printf(" --> |");
	for(i=0; i<num_x_stblz; i++) {
		printf("%d", c_lq->x_stblz_out[i]);
	}
	printf(">\n");

	
    //==================================================
	// Reset X stabilizer to Zero for future use
    //==================================================
	//printf("Reset X stabilizer to ");
	//for(i=0; i<num_x_stblz; i++)  // CNOT
	//	if(c_lq->x_stblz_out[i] == 1) { // CNOT
	//		X(QReg, c_lq->x_stblz_list[i]); // CNOT
			//printf("  Apply pauliX to X stabilizer [%d]\n", c_lq->x_stblz_list[i]);
	//	} // CNOT
	//printf("|");
	//for(i=0; i<num_x_stblz; i++) 
	//	printf("%d", M(QReg, c_lq->x_stblz_list[i]));
	//printf("> for next use\n"); 
}


//===========================================================
//
//  generate Z stabilizer
//
//===========================================================
extern void z_stblz_gen_measure(QRegister *QReg, int lq_no)
{
	int i, j;	
	int num_z_stblz;
	struct logical_qubit *c_lq;

	//printf("--------------------------------------------------\n");
	//printf("  Generate Z stabilizer\n");
	printf(" Z stabilizer : ");
	//printf("--------------------------------------------------\n");

	c_lq = &(lq[lq_no]);   // current logical qubit
	num_z_stblz =c_lq->num_z_stblz;

    //==================================================
	// Show Z stablizZr init value
    //==================================================
	//printf("Z stabilizer init value ");
	printf("|");
	for(i=0; i<num_z_stblz; i++)  {
		//printf("%d ", c_lq->z_stblz_list[i]);
		printf("%d", M(QReg, c_lq->z_stblz_list[i]));
	}
	//printf(">\n");;
	printf(">");;

    //==================================================
	// Generate Z stabilizer
    //==================================================
	//printf("Generate Z stabilizer\n");
	for(i=0; i<num_z_stblz; i++) {
		//printf("-----\n");
		for(j=0; j<4; j++) { // max 4 weight
			if(c_lq->z_stblz_scan[i][j] == -1) break; // 2 weight
			CX(QReg, c_lq->z_stblz_scan[i][j],
							c_lq->z_stblz_list[i]);
			//printf("  CNOT %d %d\n", c_lq->z_stblz_scan[i][j],
			//				c_lq->z_stblz_list[i]);
		}
		c_lq->z_stblz_out[i] = M(QReg, c_lq->z_stblz_list[i]); // CNOT
		if(c_lq->z_stblz_out[i] == 1) { // CNOT
			X(QReg, c_lq->z_stblz_list[i]); // CNOT
		} // CNOT
	}

	// store z stabilizer measurement result
	//for(i=0; i<num_z_stblz; i++) {  // CNOT
	//	c_lq->z_stblz_out[i] = M(QReg, c_lq->z_stblz_list[i]);  // CNOT
	//}  // CNOT

	//printf("Z stabilizer result |");
	printf(" --> |");
	for(i=0; i<num_z_stblz; i++) {
		printf("%d", c_lq->z_stblz_out[i]);
	}
	printf(">\n");

	
    //==================================================
	// Reset Z stabilizer to Zero for future use
    //==================================================
	//printf("Reset Z stabilizer to ");
	//for(i=0; i<num_z_stblz; i++)  // CNOT
	//	if(c_lq->z_stblz_out[i] == 1) { // CNOT
	//		X(QReg, c_lq->z_stblz_list[i]); // CNOT
			//printf("  Apply pauliX to Z stabilizer [%d]\n", c_lq->z_stblz_list[i]);
	//	} // CNOT
	//printf("|");
	//for(i=0; i<num_z_stblz; i++) 
	//	printf("%d", M(QReg, c_lq->z_stblz_list[i]));
	//printf("> for next use\n"); 
}



//===========================================================
//
//  generate X and Z stabilizer
//
//===========================================================
extern void stblz_gen_measure(QRegister *QReg, int lq_no)
{
	x_stblz_gen_measure(QReg, lq_no);
	z_stblz_gen_measure(QReg, lq_no);
}



//===========================================================
//
// Decode X error and correct
//
//===========================================================
extern void x_stblz_decode_correct(QRegister *QReg, int lq_no)
{
	struct logical_qubit *c_lq;
	int c_flavor, decoder_inx, data_pos, i;

	c_lq = &(lq[lq_no]);
	c_flavor = c_lq->flavor;

    //==================================================
	// Decode X syndrome
	//   Lookup syndrome correction table
    //==================================================
	decoder_inx = c_lq->x_stblz_out[0]*8 + c_lq->x_stblz_out[1]*4 +
				c_lq->x_stblz_out[2]*2 + c_lq->x_stblz_out[3];
    printf("X Syndrome_decoder : Flavor [%d], Lookup table index [%d]\n", 
					c_flavor, decoder_inx);

    for(i=0; i<2; i++) {
		data_pos = x_syndrome_decoder[c_flavor][decoder_inx][i];
		if(data_pos != -1 ) {
			data_pos += c_lq->data_qubit_list[0];
			printf("  Error Data qubit [%d] -->  Apply pauliZ\n", data_pos);
			Z(QReg, data_pos);
		}
    }
}


//===========================================================
//
// Decode Z error and correct
//
//===========================================================
extern void z_stblz_decode_correct(QRegister *QReg, int lq_no)
{
	struct logical_qubit *c_lq;
	int c_flavor, decoder_inx, data_pos, i;

	c_lq = &(lq[lq_no]);
	c_flavor = c_lq->flavor;

    //==================================================
	// Decode X syndrome
	//   Lookup syndrome correction table
    //==================================================
	decoder_inx = c_lq->z_stblz_out[0]*8 + c_lq->z_stblz_out[1]*4 +
				c_lq->z_stblz_out[2]*2 + c_lq->z_stblz_out[3];
    printf("Z Syndrome_decoder : Flavor [%d], Lookup table index [%d]\n", 
					c_flavor, decoder_inx);

    for(i=0; i<2; i++) {
		data_pos = z_syndrome_decoder[c_flavor][decoder_inx][i];
		if(data_pos != -1 ) {
			data_pos += c_lq->data_qubit_list[0];
			printf("  Error Data qubit [%d] -->  Apply pauliX\n", data_pos);
			X(QReg, data_pos);
		}
    }
}



//===========================================================
//
//  Decode X and Z syndrome and Correct error
//  return : if merged qubit, return joint measurement(mxx, mzz..)
//           else return 0
//
//===========================================================
extern void decode_correct(QRegister *QReg, int lq_no)
{
	int i, num_x_stblz, num_z_stblz;
	struct logical_qubit *c_lq;
	int merge_stblz[2], joint_measure;


	c_lq = &(lq[lq_no]);
	num_x_stblz = c_lq->num_x_stblz;
	num_z_stblz = c_lq->num_z_stblz;
	
	printf("X/Z stabilizer |");
	for(i=0; i<num_x_stblz; i++) 
		printf("%d", c_lq->x_stblz_out[i]);
	printf("> |"); 

	//printf("Z stabilizer |");
	for(i=0; i<num_z_stblz; i++) 
		printf("%d", c_lq->z_stblz_out[i]);
	printf(">\n"); 


	// for merged qubit : temp code
	//if(lq_no == 2)
	if(lq_no == 13)
	{
		//printf("lq_no : %d %d\n", c_lq->x_stblz_out[0], c_lq->x_stblz_out[1]);
		if(c_lq->x_stblz_out[0] == 1 && c_lq->x_stblz_out[1] == 0)
		{
			printf("added X stabilizer : 10\n");
			Z(QReg, 0);
			Z(QReg, 1);
			Z(QReg, 2);
		}
		else if(c_lq->x_stblz_out[0] == 0 && c_lq->x_stblz_out[1] == 1)
		{
			printf("added X stabilizer : 01\n");
			Z(QReg, 3);
			Z(QReg, 4);
			Z(QReg, 5);
		}
	}
	//else if(lq_no == 3)
	else if(lq_no == 14)
	{
		//printf("lq_no : %d %d\n", c_lq->z_stblz_out[0], c_lq->z_stblz_out[1]);
		if(c_lq->z_stblz_out[0] == 1 && c_lq->z_stblz_out[1] == 0)
		{
			printf("added Z stabilizer : 10\n");
			X(QReg, 1);
			X(QReg, 4);
			X(QReg, 7);
		}
		else if(c_lq->z_stblz_out[0] == 0 && c_lq->z_stblz_out[1] == 1)
		{
			printf("added Z stabilizer : 01\n");
			X(QReg, 2);
			X(QReg, 5);
			X(QReg, 8);
		}
	}
	// end of for merged qubit

	if(c_lq->num_total_qubit > 15) return;

	x_stblz_decode_correct(QReg, lq_no);
	z_stblz_decode_correct(QReg, lq_no);
}

//===========================================================
//
//  generate stabilizer and correct errors
//    return : product of added stabilizer eigenvalue in case of merged qubit
//
//===========================================================
extern int error_correct(QRegister *QReg, int lq_no, int kind)
{
	struct logical_qubit *c_lq;

	//int x_stblz_out[4], z_stblz_out[4];

	//printf("========================================================\n");
	printf("--------------------------------------------------------\n");
	printf("Error correction : logical qubit #%d\n", lq_no);
	printf("--------------------------------------------------------\n");

	stblz_gen_measure(QReg, lq_no);
	if(kind == 0)
		decode_correct(QReg, lq_no);

	c_lq = &(lq[lq_no]);
	if(lq_no == 2)
	{
		if( (c_lq->x_stblz_out[0] == 0 && c_lq->x_stblz_out[1] == 1) ||
		    (c_lq->x_stblz_out[0] == 1 && c_lq->x_stblz_out[1] == 0))
			return -1;
		else
			return 1;
	}

	if(lq_no == 13)
	{
		if( (c_lq->x_stblz_out[0] == 0 && c_lq->x_stblz_out[1] == 1) ||
		    (c_lq->x_stblz_out[0] == 1 && c_lq->x_stblz_out[1] == 0))
			return -1;
		else
			return 1;
	}


	if(lq_no == 14)
	{
		if( (c_lq->z_stblz_out[0] == 0 && c_lq->z_stblz_out[1] == 1) ||
		    (c_lq->z_stblz_out[0] == 1 && c_lq->z_stblz_out[1] == 0))
			return -1;
		else
			return 1;
	}



	//stblz(sc17_q, x_stblz, z_stblz);
	//measure_stblz(sc17_q, x_stblz, z_stblz);
	//decode_correct(sc17_q, x_stblz, z_stblz);

	//printf("========================================================\n");
	printf("--------------------------------------------------------\n");

	return 0;
}

class CNOT_OH {
private:
	QRegister *QReg;	

public:
	CNOT_OH(void) {
		QReg = new QRegister(28);
	}

	~CNOT_OH(void) {
		delete QReg;
	}

public:
	void run(void) {
		int i, j, k, mxx, mzz, mx, a, b, c;

		printf("===================================================================\n");
		printf(" Logical qubit information \n");
		for(i=0; i<3; i++) 
		{
			printf("-------------------------------------------------------------------\n");
			printf("Logical qubit #%d\n", i);
			printf("%d %d %d %d %d\n", lq[i].flavor, lq[i].num_total_qubit, 
					lq[i].num_data_qubit, lq[i].num_x_stblz, lq[i].num_z_stblz);

			printf("data qubit list : ");
			for(j=0; j<lq[i].num_data_qubit; j++)
				printf("%d ", lq[i].data_qubit_list[j]);
			printf("\n");
	
			printf("X stabilizer list : ");
			for(j=0; j<lq[i].num_x_stblz; j++)
				printf("%d ", lq[i].x_stblz_list[j]);
			printf("\n");
			
			printf("Z stabilizer list : ");
			for(j=0; j<lq[i].num_z_stblz; j++)
				printf("%d ", lq[i].z_stblz_list[j]);
			printf("\n");
	
			printf("X stabilizer scan : ");
			for(j=0; j<lq[i].num_x_stblz; j++) {
				for(k=0; k<4; k++) {
					if(lq[i].x_stblz_scan[j][k] == -1) break;
					printf("%d ", lq[i].x_stblz_scan[j][k]);
				}
				printf(" - ");
			}
			printf("\n");
	
			printf("Z stabilizer scan : ");
			for(j=0; j<lq[i].num_z_stblz; j++) {
				for(k=0; k<4; k++) {
					if(lq[i].z_stblz_scan[j][k] == -1) break;
					printf("%d ", lq[i].z_stblz_scan[j][k]);
				}
				printf(" - ");
			}
			printf("\n");
	
			printf("X stabilizer measurement out : ");
			for(j=0; j<lq[i].num_x_stblz; j++) 
				printf("%d ", lq[i].x_stblz_out[j]);
			printf("\n");
	
			printf("Z stabilizer measurement out : ");
			for(j=0; j<lq[i].num_z_stblz; j++) 
				printf("%d ", lq[i].z_stblz_out[j]);
			printf("\n");
		}
		printf("===================================================================\n");


		printf("\n");
		printf("===================================================================\n");
		printf(" Lattice Surgery CNOT\n");
		printf("LQ#10 (A), LQ#11(T), LQ#12(C)\n");
		printf("===================================================================\n");

		printf("\n");
		printf("===================================================================\n");
		printf(" Init Logical Qubit #10 (Ancilla) to |0>\n");
		printf("===================================================================\n");
		error_correct(QReg, 10, 0);
	//	print_statevector_lq(QReg, 0, 1);

		printf("\n");
		printf("====================================================================\n");
		printf("Init Logical Qubit #11 (Target) to |->\n");
		printf("====================================================================\n");
		for(i=9; i<=17; i++)
			H(QReg, i); 
		error_correct(QReg, 11, 0);
		logical_z_operator(QReg, 11);

		printf("\n");
		printf("====================================================================\n");
		printf("Init Logical Qubit #12 (Control) to |+>\n");
		printf("====================================================================\n");
		for(i=18; i<=26; i++)
			H(QReg, i); 
		error_correct(QReg, 12, 0);
		//logical_x_operator(QReg, 12);
		//logical_z_operator(QReg, 12);

		printf("----------------------\n");
		printf(" Initialization\n");
		printf("----------------------\n");
		//print_statevector_lq(QReg, 10, 1);
		//print_statevector_lq(QReg, 11, 1);
		//print_statevector_lq(QReg, 12, 1);

		//AT merge
		printf("----------------------\n");
		printf(" AT merge\n");
		printf("----------------------\n");

		mxx = error_correct(QReg, 13, 1);

		//print_statevector_lq(QReg, 10, 1);
		//print_statevector_lq(QReg, 11, 1);
		//print_statevector_lq(QReg, 12, 1);
		//print_statevector_lq(QReg, 13, 1);

		//AT split
		printf("----------------------\n");
		printf(" AT split\n");
		printf("----------------------\n");

		error_correct(QReg, 10, 0);
		error_correct(QReg, 11, 0);

		//print_statevector_lq(QReg, 10, 1);
		//print_statevector_lq(QReg, 11, 1);
		//print_statevector_lq(QReg, 12, 1);
		//print_statevector_lq(QReg, 13, 1);


		//CA merge
		printf("----------------------\n");
		printf(" CA merge\n");
		printf("----------------------\n");

		mzz= error_correct(QReg, 14, 1);

		//print_statevector_lq(QReg, 10, 1);
		//print_statevector_lq(QReg, 11, 1);
		//print_statevector_lq(QReg, 12, 1);
		//print_statevector_lq(QReg, 14, 1);

		//CA split
		printf("----------------------\n");
		printf(" CA split\n");
		printf("----------------------\n");

		error_correct(QReg, 10, 0);
		error_correct(QReg, 12, 0);

		//print_statevector_lq(QReg, 10, 1);
		//print_statevector_lq(QReg, 11, 1);
		//print_statevector_lq(QReg, 12, 1);
		//print_statevector_lq(QReg, 14, 1);


		printf("----------------------\n");
		printf(" measure\n");
		printf("----------------------\n");
		for(i=0; i<=8; i++)
			H(QReg, i); 
		mx = measure_lq(QReg, 10);
		//print_statevector_lq(QReg, 10, 1);
		//print_statevector_lq(QReg, 11, 1);
		//print_statevector_lq(QReg, 12, 1);

		a = (mxx==1)?0:1;
		b = (mzz==1)?0:1;
		c = (mx==1)?0:1;


		printf("mxx:a[%d, %d] : mzz:b[%d, %d] : mx:c[%d, %d]\n", mxx, a, mzz, b, mx, c);
	
		if((a+c) == 1) logical_z_operator(QReg, 12);
		if(c == 1) logical_z_operator(QReg, 10);
		if(b == 1) logical_x_operator(QReg, 11);

		//print_statevector_lq(QReg, 10, 1);
		//print_statevector_lq(QReg, 11, 1);
		//print_statevector_lq(QReg, 12, 1);
		//print_statevector_lq(QReg, 15, 1);
	
		//print_statevector_lq(QReg, 11, 1);
		//print_statevector_lq(QReg, 12, 1);
	

		for(i=9; i<=17; i++)
			H(QReg, i); 
		measure_lq(QReg, 11);

		QReg->dump();

		for(i=18; i<=26; i++)
			H(QReg, i); 
		measure_lq(QReg, 12);
	}
};

int main(int argc, char **argv)
{
	CNOT_OH *OH = new CNOT_OH();
	OH->run();
}
