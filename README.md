# Gadget lyman alpha subhalo potential

Lyman alpha and subhalo potential output reproduced from P-Gadget3 including subhalo finder (subfinder).

Many more options can be found in `Makefile`.

% Gadget

@ARTICLE{springel2001,
   author = {{Springel}, V. and {Yoshida}, N. and {White}, S.~D.~M.},
    title = "{GADGET: a code for collisionless and gasdynamical cosmological simulations}",
  journal = {\na},
   eprint = {astro-ph/0003162},
     year = 2001,
    month = apr,
   volume = 6,
    pages = {79-117},
      doi = {10.1016/S1384-1076(01)00042-2},
   adsurl = {http://adsabs.harvard.edu/abs/2001NewA....6...79S},
  adsnote = {Provided by the SAO/NASA Astrophysics Data System}
}

% Subfinder

@ARTICLE{springel2001subfind,
   author = {{Springel}, V. and {White}, S.~D.~M. and {Tormen}, G. and {Kauffmann}, G.
	},
    title = "{Populating a cluster of galaxies - I. Results at [formmu2]z=0}",
  journal = {\mnras},
   eprint = {astro-ph/0012055},
 keywords = {GALAXIES: CLUSTERS: GENERAL, GALAXIES: FORMATION, DARK MATTER},
     year = 2001,
    month = dec,
   volume = 328,
    pages = {726-750},
      doi = {10.1046/j.1365-8711.2001.04912.x},
   adsurl = {http://cdsads.u-strasbg.fr/abs/2001MNRAS.328..726S},
  adsnote = {Provided by the SAO/NASA Astrophysics Data System}
}

## Install

Make sure either IntelMPI or OpenMPI has been installed.
```
$ git clone https://github.com/OMGitsHongyu/Gadget_lya_subhalo_potential.git
$ cd Gadget_lya_subhalo_potential/
$ make
```

## Usage

Turn on/off options in Makefile and compile.

This version could be used on National Energy Research Scientific Computing Center (NERSC) machines (Edison, Cori).

To submit a job based on `sbatch`, look into `./submission/`:

Change parameters on `./submission/run_params.txt`.
Submit a batch job using `./submission/sbatch_submission.sh`, note `qsub` should have a similar fashion.
