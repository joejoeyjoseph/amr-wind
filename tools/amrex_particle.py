# -*- coding: utf-8 -*-

"""\
AMReX Particle Data Processor
-----------------------------

This module provides a python class :class:`AmrexParticleFile` that can parse
the sampling data written out by AMR-Wind using the native AMReX binary format.

"""

from pathlib import Path
import numpy as np
import pandas as pd

class AmrexParticleFile:
    """AmrexParticleFile reader

    Loads AMReX Particle data in binary format for a single timestep
    """

    @classmethod
    def load(cls, time_index, tower_num, label="sampling", root_dir="post_processing"):
        """
        Args:
            time_index (int): Time index to load
            label (str): Label used for this set of probes
            root_dir (path): Path to the post_processing directory
        """
        fpath = Path(root_dir) / ("%s%05d"%(label, time_index))
        return cls(fpath, tower_num)

    def __init__(self, pdir, tower_num):
        """
        Args:
            pdir (path): Directory path
        """
        self.pdir = Path(pdir)
        assert self.pdir.exists()

        self.tower_num = tower_num

    def __call__(self):
        """Parse the header and load all binary files

        Returns:
            DataFrame: A pandas dataframe with data for all the particles
        """
        if not hasattr(self, "df"):
            self.parse_header()
            self.load_binary_data()
        return self.df

    def parse_header(self):
        """Parse the header and load metadata"""
        with open(self.pdir / "Header", 'r') as fh:
            # Skip first line
            fh.readline()
            self.ndim = int(fh.readline().strip())
            self.num_reals = int(fh.readline().strip())
            self.real_var_names = [fh.readline().strip() for _ in
                                   range(self.num_reals)]
            self.num_ints = int(fh.readline().strip())
            self.int_var_names = [fh.readline().strip() for _ in
                                  range(self.num_ints)]
            #print(self.ndim, self.num_reals, self.num_ints)
            # skip flag
            fh.readline()
            self.num_particles = int(fh.readline().strip())
            self.next_id = int(fh.readline().strip())
            self.finest_level = int(fh.readline().strip())
            #print(self.finest_level, self.num_particles, self.next_id)

            self.num_grids = np.empty((self.finest_level+1,), dtype=np.int)
            self.grid_info = []
            for lev in range(self.finest_level+1):
                ginfo = []
                self.num_grids[lev] = int(fh.readline().strip())
                for _ in range(self.num_grids[lev]):
                    ginfo.append(
                        [int(ix) for ix in fh.readline().strip().split()])
                self.grid_info.append(ginfo)

    def load_binary_data(self):
        """Read binary data into memory"""
        self.real_data = np.empty((self.num_particles,
                                   self.ndim+self.num_reals), dtype=np.float)
        self.int_data = np.empty((self.num_particles,
                                  self.num_ints), dtype=np.int)

        idata = self.int_data
        #print(idata.shape)
        rdata = self.real_data
        nints = self.num_ints + 2
        nreals = self.num_reals + self.ndim
        #print(self.grid_info)
        for lev, ginfo in enumerate(self.grid_info):
            #print('lev')
            #print(lev)
            bfiles = {}
            for idx, npts, offset in ginfo:
                #print('idx')
                #print(idx, npts, offset)
                if npts < 1:
                    continue
                fname = self.bin_file_name(lev, idx)
                fstr = str(fname)
                if fstr not in bfiles:
                    bfiles[fstr] = open(fname, 'rb')
                fh = bfiles[fstr]
                #print('fh', fh)
                fh.seek(offset)
                ivals = np.fromfile(fh, dtype=np.int32, count=nints*npts)
                rvals = np.fromfile(fh, dtype=np.float, count=nreals*npts)
                #print(ivals)
                #print(ivals.shape)
                #print(nints, npts)
                #print('#####')

                for i, ii in enumerate(range(0, nints * npts, nints)):
                    #print(i, ii)
                    #print(ivals)
                    #print(ivals.shape)
                    #print(idata.shape)
                    #pidx = ivals[ii + 2]
                    pidx = ivals[ii + 2]-self.tower_num+1
                    #print(pidx)
                    idata[pidx, 0] = pidx
                    idata[pidx, 1] = ivals[ii + 3]
                    idata[pidx, 2] = ivals[ii + 4]
                    #print('*****')

                    offset = i * nreals
                    for jj in range(nreals):
                        rdata[pidx, jj] = rvals[offset + jj]
            for fh in bfiles.values():
                fh.close()

        idict = {key: idata[:, i] for i, key in enumerate(self.int_var_names)}
        rvar_names = "xco yco zco".split() + self.real_var_names
        rdict = {key: rdata[:, i] for i, key in enumerate(rvar_names)}
        idict.update(rdict)
        self.df = pd.DataFrame(idict, index=idata[:,0])
        #print('%%%%%')
        #print(self.df.shape)

    def bin_file_name(self, lev, idx):
        """Return the name of the binary file"""
        lvl_name = "Level_%d"%lev
        fname = "DATA_%05d"%idx
        return (self.pdir / lvl_name / fname)

    def __repr__(self):
        return "<%s: %s>"%(
            self.__class__.__name__, self.pdir.stem)
