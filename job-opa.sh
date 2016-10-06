#!/bin/tcsh
#$ -j n
#$ -cwd
#$ -pe mvapich2 8
#$ -m be
#$ -M volker@mpa-garching.mpg.de
#$ -N test
#


#module load mvapich2-1.2-sdr-intel/11.0
module load mvapich2-1.2-sdr-gnu/4.1.2


mpiexec -np $NSLOTS  ./P-Gadget3  param.txt 

if [ -f ../cont ] 
 then
#    rm -f "../cont"
#    qsub job-opa.sh
fi






