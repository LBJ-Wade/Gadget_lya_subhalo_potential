#!/bin/bash -l

#SBATCH -p regular
#SBATCH --qos premium
#SBATCH -N 64
#SBATCH -n 512
#SBATCH -t 36:00:00

echo " "
echo "Job started on `hostname` at `date`"
module load impi
export I_MPI_PMI_LIBRARY=/opt/slurm/default/lib/libpmi.so.0
export I_MPI_COMPATIBILITY=3
srun -n 512 ~/nbody/lya_p_gadget/P-Gadget3 run1.txt
echo " "
echo "Job ended on `hostname` at `date`"
