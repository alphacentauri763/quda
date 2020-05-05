/* Utilities for testing QIO */
#include <stdio.h>
#include <string.h>
#include <qio_util.h>

void print_m(suN_matrix *a)
{
  for (int i = 0; i < NCLR; i++) {
    printf("%f %f %f %f %f %f\n", a->e[i][0].re, a->e[i][0].im, a->e[i][1].re, a->e[i][1].im, a->e[i][2].re,
           a->e[i][2].im);
  }
}

void vfill_m(suN_matrix *a, int coords[], int rank)
{
  for (int j = 0; j < NCLR; j++)
    for (int i = 0; i < NCLR; i++) {
      a->e[j][i].re = 0.0;
      a->e[j][i].im = 0.0;
    }

  for (int j = 0; j < NCLR; j++)
    a->e[j][j].re = 100*rank + coords[0] +
      lattice_size[0]*(coords[1] + lattice_size[1]*
		       (coords[2] + lattice_size[2]*coords[3]));
}

void vset_M(suN_matrix *field[], int count)
{
  int x[4];

  for (int i = 0; i < count; i++)
    for (x[3] = 0; x[3] < lattice_size[3]; x[3]++)
      for (x[2] = 0; x[2] < lattice_size[2]; x[2]++)
        for (x[1] = 0; x[1] < lattice_size[1]; x[1]++)
          for (x[0] = 0; x[0] < lattice_size[0]; x[0]++) {
            if (quda_node_number(x) == quda_this_node) {
              int index = quda_node_index(x);
              vfill_m(field[i] + index, x, i);
            }
          }
}

int vcreate_M(suN_matrix *field[], int count)
{
  /* Create an output field */
  for (int i = 0; i < count; i++) {
    field[i] = (suN_matrix *)malloc(sizeof(suN_matrix) * quda_num_sites(quda_this_node));
    if(field[i] == NULL){
      printf("vcreate_M(%d): Can't malloc field\n", quda_this_node);
      return 1;
    }
  }

  return 0;
}

/* destroy array of fields */
void vdestroy_M (suN_matrix *field[], int count)
{
  for (int i = 0; i < count; i++) free(field[i]);
}

float vcompare_M(suN_matrix *fielda[], suN_matrix *fieldb[], int count)
{
  float sum2 = 0;

  for (int k = 0; k < count; k++)
    for (int m = 0; m < quda_num_sites(quda_this_node); m++) {
      for (int j = 0; j < NCLR; j++)
        for (int i = 0; i < NCLR; i++) {
          float diff = fielda[k][m].e[j][i].re - fieldb[k][m].e[j][i].re;
          sum2 += diff * diff;
          diff = fielda[k][m].e[j][i].im - fieldb[k][m].e[j][i].im;
          sum2 += diff * diff;
        }
    }

  /* Global sum */
  QMP_sum_float(&sum2);
  return sum2;
}

/* Copy a subset */
int inside_subset(int x[], int lower[], int upper[])
{
  int status = 1;

  for (int i = 0; i < 4; i++)
    if(lower[i] > x[i] || upper[i] < x[i]){
      status = 0;
      break;
    }

  return status;
}
