#ifdef SUPPORT_HDF5

#include "Copyright.h"
#include "GAMER.h"
#include "CUFLU.h"
#ifdef GRAVITY
#include "CUPOT.h"
#endif
#include "HDF5_Typedef.h"
#include <ctime>

void FillIn_KeyInfo  (   KeyInfo_t &KeyInfo   );
void FillIn_Makefile (  Makefile_t &Makefile  );
void FillIn_SymConst (  SymConst_t &SymConst  );
void FillIn_InputPara( InputPara_t &InputPara );

static void GetCompound_KeyInfo  ( hid_t &H5_TypeID );
static void GetCompound_Makefile ( hid_t &H5_TypeID );
static void GetCompound_SymConst ( hid_t &H5_TypeID );
static void GetCompound_InputPara( hid_t &H5_TypeID );



/*======================================================================================================
Data structure:
/ -> |
     | -> Info group -> | -> InputPara dset (compound)
     |                  | -> KeyInfo   dset (compound)
     |                  | -> Makefile  dset (compound)
     |                  | -> SymConst  dset (compound)
     |
     | -> Tree group -> | -> Corner  dset -> Cvt2Phy attrs
     |                  | -> LBIdx   dset
     |                  | -> Father  dset
     |                  | -> Son     dset
     |                  | -> Sibling dset
     |
     | -> Data group -> | -> Dens
                        | -> ...
                        | -> ...
======================================================================================================*/



/*======================================================================================================
h5py usage (with python 2):
1. Load file: f=h5py.File("Data_000000", "r")
2. Shows the names of all groups: list(f) or f.keys()
3. Show the names of all attributes: list(f['Tree']['Corner'].attrs) or f['Tree']['Corner'].attrs.keys()
4. Show a specific attribute: f['Tree']['Corner'].attrs['Cvt2Phy']
5. Show all variables in a compound variable: f['Info']['KeyInfo'].dtype
6. Show the value of a dataset: f['Tree']['Corner'].value
7. Show density of a patch with a global ID (GID) 1234: f['Data']['Dens'][1234]
8. Show density at a specific cell [1][2][3]: f['Data']['Dens'][1234][1][2][3]
======================================================================================================*/



/*======================================================================================================
Procedure for outputting new variables:
1. Add the new variable into one of the data structures (XXX) defined in "HDF5_Typedef.h"
2. Edit "GetCompound_XXX" to insert the new variables into the compound datatype
3. Edit "FillIn_XXX" to fill in the new variables
4. Edit "Check_XXX" in "Init_Restart_HDF5.cpp" to load and compare the new variables
5. Modify FormatVersion and CodeVersion
======================================================================================================*/




//-------------------------------------------------------------------------------------------------------
// Function    :  Output_DumpData_Total_HDF5 (FormatVersion = 2101)
// Description :  Output all simulation data in the HDF5 format, which can be used as a restart file
//                or loaded by YT
//
// Note        :  1. Please refer to the "Data structure" described on the top of this file 
//                2. Patch IDs stored in the HDF5 output are always GID (global identification) instead of
//                   PID (patch identification). Unlike PID, which always starts from 0 at different ranks
//                   and different levels, GID is unique among all patches at all ranks and all levels
//                   --> Each patch has an unique GID
//                3. Both "Father, Son, and Sibling[26]" are GID instead of PID
//                4. Currently we always use HDF5 NATVIE datatypes for both memory and dataset
//                5. All arrays in the "Tree" group (e.g., Corner, LBIdx, ...) and the "Data" group (e.g., Dens, MomX, ...)
//                   have been sorted according to GID
//                   --> Moreover, currently we store all patches at the same level together
//                   --> A higher-level patch always has a larger GID than a lower-level patch
//                5. LBIdx dataset stores the LB_Idx of all patches sorted by their GIDs
//                   --> This list will be created even when LOAD_BALANCE is not turned on
//                       so that a serial output can be loaded by a parallel run easily
//                6. All C structures (e.g., "KeyInfo, SymConst, ..." are stored as a single (scalar)
//                   compound datetype
//                   --> Better update h5py to version >= 2.3.0 to properly read it in python
//                   --> Corresponding C structures are defined in "HDF5_Typedef.h"
//                7. It seems that h5py still have problem for "modifying" the loaded data. But reading data is fine.
//                8. The "H5T_GAMER_REAL" datatype will be mapped to "H5T_NATIVE_DOUBLE / H5T_NATIVE_FLOAT" if 
//                   FLOAT8 is on / off
//                9. It is found that in the parallel environment each rank must try to "synchronize" the HDF5 file
//                   before opening the existed file and add data
//                   --> To achieve that, please always invoke "SyncHDF5File" before calling "H5Fopen"
//                   --> "SyncHDF5File" is defined in "HDF5_Typedef.h", which simply openes the file 
//                       with the appending mode and then closes it immediately
//
// Parameter   :  FileName : Name of the output file
//-------------------------------------------------------------------------------------------------------
void Output_DumpData_Total_HDF5( const char *FileName )
{  

   if ( MPI_Rank == 0 )    Aux_Message( stdout, "%s (DumpID = %d) ...\n", __FUNCTION__, DumpID );


// check the synchronization
   for (int lv=1; lv<NLEVEL; lv++)
      if ( NPatchTotal[lv] != 0 )   Mis_CompareRealValue( Time[0], Time[lv], __FUNCTION__, true );


// check if the targeted file already exists
   if ( Aux_CheckFileExist(FileName)  &&  MPI_Rank == 0 )
      Aux_Message( stderr, "WARNING : file \"%s\" already exists and will be overwritten !!\n", FileName );


// 1. gather the number of patches at different MPI ranks and set the corresponding GID offset
   int (*NPatchAllRank)[NLEVEL] = new int [MPI_NRank][NLEVEL];
   int NPatchLocal[NLEVEL], NPatchAllLv=0, GID_Offset[NLEVEL], GID_LvStart[NLEVEL];

   for (int lv=0; lv<NLEVEL; lv++)  NPatchLocal[lv] = amr->NPatchComma[lv][1];

   MPI_Allgather( NPatchLocal, NLEVEL, MPI_INT, NPatchAllRank[0], NLEVEL, MPI_INT, MPI_COMM_WORLD ); 

   for (int lv=0; lv<NLEVEL; lv++)
   {
      GID_Offset[lv] = 0;

      for (int r=0; r<MPI_Rank; r++)      GID_Offset[lv] += NPatchAllRank[r][lv];

      for (int FaLv=0; FaLv<lv; FaLv++)   GID_Offset[lv] += NPatchTotal[FaLv];

      NPatchAllLv += NPatchTotal[lv];

      GID_LvStart[lv] = ( lv == 0 ) ? 0 : GID_LvStart[lv-1] + NPatchTotal[lv-1];
   }



// 2. prepare all HDF5 variables
   hsize_t H5_SetDims_LBIdx, H5_SetDims_Cr[2], H5_SetDims_Fa, H5_SetDims_Son, H5_SetDims_Sib[2], H5_SetDims_Field[4];
   hsize_t H5_MemDims_Field[4], H5_Count_Field[4], H5_Offset_Field[4];
   hid_t   H5_MemID_Field;
   hid_t   H5_FileID, H5_GroupID_Info, H5_GroupID_Tree, H5_GroupID_Data;
   hid_t   H5_SetID_LBIdx, H5_SetID_Cr, H5_SetID_Fa, H5_SetID_Son, H5_SetID_Sib, H5_SetID_Field;
   hid_t   H5_SetID_KeyInfo, H5_SetID_Makefile, H5_SetID_SymConst, H5_SetID_InputPara;
   hid_t   H5_SpaceID_Scalar, H5_SpaceID_LBIdx, H5_SpaceID_Cr, H5_SpaceID_Fa, H5_SpaceID_Son, H5_SpaceID_Sib, H5_SpaceID_Field;
   hid_t   H5_TypeID_Com_KeyInfo, H5_TypeID_Com_Makefile, H5_TypeID_Com_SymConst, H5_TypeID_Com_InputPara;
   hid_t   H5_DataCreatePropList;
   hid_t   H5_AttID_Cvt2Phy;
   herr_t  H5_Status;

// 2-1. do NOT write fill values to any dataset for higher I/O performance
   H5_DataCreatePropList = H5Pcreate( H5P_DATASET_CREATE );
   H5_Status             = H5Pset_fill_time( H5_DataCreatePropList, H5D_FILL_TIME_NEVER );
   
// 2-2. create the "compound" datatype
   GetCompound_KeyInfo  ( H5_TypeID_Com_KeyInfo   );
   GetCompound_Makefile ( H5_TypeID_Com_Makefile  );
   GetCompound_SymConst ( H5_TypeID_Com_SymConst  );
   GetCompound_InputPara( H5_TypeID_Com_InputPara );

// 2-3. create the "scalar" dataspace
   H5_SpaceID_Scalar = H5Screate( H5S_SCALAR );



// 3. output the simulation information 
   if ( MPI_Rank == 0 )
   {
//    3-1. collect all information to be recorded
      KeyInfo_t   KeyInfo;
      Makefile_t  Makefile;
      SymConst_t  SymConst;
      InputPara_t InputPara;

      FillIn_KeyInfo  ( KeyInfo   );
      FillIn_Makefile ( Makefile  );
      FillIn_SymConst ( SymConst  );
      FillIn_InputPara( InputPara );


//    3-2. create the HDF5 file (overwrite the existing file)
      H5_FileID = H5Fcreate( FileName, H5F_ACC_TRUNC, H5P_DEFAULT, H5P_DEFAULT );

      if ( H5_FileID < 0 )    Aux_Error( ERROR_INFO, "failed to create the HDF5 file \"%s\" !!\n", FileName );


//    3-3. write the simulation info (note: dataset doesn't support VL datatype when the fill value is not defined)
      H5_GroupID_Info = H5Gcreate( H5_FileID, "Info", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT );
      if ( H5_GroupID_Info < 0 )    Aux_Error( ERROR_INFO, "failed to create the group \"%s\" !!\n", "Info" );

//    3-3-1. KeyInfo
      H5_SetID_KeyInfo   = H5Dcreate( H5_GroupID_Info, "KeyInfo", H5_TypeID_Com_KeyInfo, H5_SpaceID_Scalar,
                                      H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT );
      if ( H5_SetID_KeyInfo < 0 )   Aux_Error( ERROR_INFO, "failed to create the dataset \"%s\" !!\n", "KeyInfo" );
      H5_Status          = H5Dwrite( H5_SetID_KeyInfo, H5_TypeID_Com_KeyInfo, H5S_ALL, H5S_ALL, H5P_DEFAULT, &KeyInfo );
      H5_Status          = H5Dclose( H5_SetID_KeyInfo );

//    3-3-2. Makefile
      H5_SetID_Makefile  = H5Dcreate( H5_GroupID_Info, "Makefile", H5_TypeID_Com_Makefile, H5_SpaceID_Scalar,
                                      H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT );
      if ( H5_SetID_Makefile < 0 )  Aux_Error( ERROR_INFO, "failed to create the dataset \"%s\" !!\n", "Makefile" );
      H5_Status          = H5Dwrite( H5_SetID_Makefile, H5_TypeID_Com_Makefile, H5S_ALL, H5S_ALL, H5P_DEFAULT, &Makefile );
      H5_Status          = H5Dclose( H5_SetID_Makefile );

//    3-3-3. SymConst
      H5_SetID_SymConst  = H5Dcreate( H5_GroupID_Info, "SymConst", H5_TypeID_Com_SymConst, H5_SpaceID_Scalar,
                                      H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT );
      if ( H5_SetID_SymConst < 0 )  Aux_Error( ERROR_INFO, "failed to create the dataset \"%s\" !!\n", "SymConst" );
      H5_Status          = H5Dwrite( H5_SetID_SymConst, H5_TypeID_Com_SymConst, H5S_ALL, H5S_ALL, H5P_DEFAULT, &SymConst );
      H5_Status          = H5Dclose( H5_SetID_SymConst );

//    3-3-4. InputPara
      H5_SetID_InputPara = H5Dcreate( H5_GroupID_Info, "InputPara", H5_TypeID_Com_InputPara, H5_SpaceID_Scalar,
                                      H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT );
      if ( H5_SetID_InputPara < 0 ) Aux_Error( ERROR_INFO, "failed to create the dataset \"%s\" !!\n", "InputPara" );
      H5_Status          = H5Dwrite( H5_SetID_InputPara, H5_TypeID_Com_InputPara, H5S_ALL, H5S_ALL, H5P_DEFAULT, &InputPara );
      H5_Status          = H5Dclose( H5_SetID_InputPara );

      H5_Status = H5Gclose( H5_GroupID_Info );
      H5_Status = H5Fclose( H5_FileID );
   } // if ( MPI_Rank == 0 )



// 4. output the AMR tree structure (father, son, sibling, LBIdx, and corner sorted by GID)
   long *LBIdxList_Local[NLEVEL], *LBIdxList_AllLv;
   int  (*CrList_Local[NLEVEL])[3], (*CrList_AllLv)[3];
   int  *FaList_Local[NLEVEL], *FaList_AllLv;
   int  *SonList_Local[NLEVEL], *SonList_AllLv;
   int  (*SibList_Local[NLEVEL])[26], (*SibList_AllLv)[26];

   long *LBIdxList_Sort[NLEVEL];
   int  *LBIdxList_Sort_IdxTable[NLEVEL];

   int   MyGID, FaPID, FaGID, FaLv, SonPID, SonGID, SonLv, SibPID, SibGID, MatchIdx;
   long  FaLBIdx, SonLBIdx, SibLBIdx;
   int  *SonCr=NULL, *SibCr=NULL;

   int   RecvCount_LBIdx[MPI_NRank], RecvDisp_LBIdx[MPI_NRank], RecvCount_Cr[MPI_NRank], RecvDisp_Cr[MPI_NRank];
   int   RecvCount_Fa[MPI_NRank], RecvDisp_Fa[MPI_NRank], RecvCount_Son[MPI_NRank], RecvDisp_Son[MPI_NRank];
   int   RecvCount_Sib[MPI_NRank], RecvDisp_Sib[MPI_NRank];

// 4-1. allocate lists
   if ( MPI_Rank == 0 )
   {
      LBIdxList_AllLv = new long [ NPatchAllLv ];
      CrList_AllLv    = new int  [ NPatchAllLv ][3];
      FaList_AllLv    = new int  [ NPatchAllLv ];
      SonList_AllLv   = new int  [ NPatchAllLv ];
      SibList_AllLv   = new int  [ NPatchAllLv ][26];
   }

   for (int lv=0; lv<NLEVEL; lv++)
   {
      LBIdxList_Local        [lv] = new long [ amr->NPatchComma[lv][1] ];
      CrList_Local           [lv] = new int  [ amr->NPatchComma[lv][1] ][3];
      FaList_Local           [lv] = new int  [ amr->NPatchComma[lv][1] ];
      SonList_Local          [lv] = new int  [ amr->NPatchComma[lv][1] ];
      SibList_Local          [lv] = new int  [ amr->NPatchComma[lv][1] ][26];

      LBIdxList_Sort         [lv] = new long [ NPatchTotal[lv] ];
      LBIdxList_Sort_IdxTable[lv] = new int  [ NPatchTotal[lv] ];
   }


// 4-2. collect and sort LBIdx from all ranks
   for (int lv=0; lv<NLEVEL; lv++)
   {
      for (int r=0; r<MPI_NRank; r++)
      {
         RecvCount_LBIdx[r] = NPatchAllRank[r][lv];
         RecvDisp_LBIdx [r] = ( r == 0 ) ? 0 : RecvDisp_LBIdx[r-1] + RecvCount_LBIdx[r-1];
      }

      for (int PID=0; PID<amr->NPatchComma[lv][1]; PID++)
         LBIdxList_Local[lv][PID] = amr->patch[0][lv][PID]->LB_Idx;

//    all ranks need to get LBIdxList_Sort since we will use it to calculate GID
      MPI_Allgatherv( LBIdxList_Local[lv], amr->NPatchComma[lv][1], MPI_LONG,
                      LBIdxList_Sort[lv], RecvCount_LBIdx, RecvDisp_LBIdx, MPI_LONG,
                      MPI_COMM_WORLD );
   } // for (int lv=0; lv<NLEVEL; lv++)

// store in the AllLv array before sorting
   if ( MPI_Rank == 0 )
   {
      MyGID = 0;

      for (int lv=0; lv<NLEVEL; lv++)
      for (int PID=0; PID<NPatchTotal[lv]; PID++)
         LBIdxList_AllLv[ MyGID++ ] = LBIdxList_Sort[lv][PID];
   }

// sort list and get the corresponding index table (for calculating GID later)
   for (int lv=0; lv<NLEVEL; lv++)
      Mis_Heapsort( NPatchTotal[lv], LBIdxList_Sort[lv], LBIdxList_Sort_IdxTable[lv] );


// 4-3. store the local tree
   for (int lv=0; lv<NLEVEL; lv++)
   {
      for (int PID=0; PID<amr->NPatchComma[lv][1]; PID++)
      {
//       4-3-1. LBIdx (set already)
//       LBIdxList_Local[lv][PID] = amr->patch[0][lv][PID]->LB_Idx;


//       4-3-2. corner
         for (int d=0; d<3; d++)
         CrList_Local[lv][PID][d] = amr->patch[0][lv][PID]->corner[d];


//       4-3-3. father GID
         FaPID = amr->patch[0][lv][PID]->father;
         FaLv  = lv - 1;

//       no father (only possible for the root patches)
         if ( FaPID < 0 )
         {
#           ifdef DEBUG_HDF5
            if ( lv != 0 )       Aux_Error( ERROR_INFO, "Lv %d, PID %d, FaPID %d < 0 !!\n", lv, PID, FaPID );
            if ( FaPID != -1 )   Aux_Error( ERROR_INFO, "Lv %d, PID %d, FaPID %d < 0 but != -1 !!\n", lv, PID, FaPID );
#           endif

            FaGID = FaPID;
         }

//       father patch is a real patch
         else if ( FaPID < amr->NPatchComma[FaLv][1] )
            FaGID = FaPID + GID_Offset[FaLv];

//       father patch is a buffer patch (only possible in LOAD_BALANCE)
         else // (FaPID >= amr->NPatchComma[FaLv][1] )
         {
#           ifdef DEBUG_HDF5
#           ifndef LOAD_BALANCE
            Aux_Error( ERROR_INFO, "Lv %d, PID %d, FaPID %d >= NRealFaPatch %d (only possible in LOAD_BALANCE) !!\n",
                       lv, PID, FaPID, amr->NPatchComma[FaLv][1] );
#           endif

            if ( FaPID >= amr->num[FaLv] )
            Aux_Error( ERROR_INFO, "Lv %d, PID %d, FaPID %d >= total number of patches %d !!\n",
                       lv, PID, FaPID, amr->num[FaLv] );
#           endif // DEBUG_HDF5

            FaLBIdx = amr->patch[0][FaLv][FaPID]->LB_Idx;

            Mis_Matching_int( NPatchTotal[FaLv], LBIdxList_Sort[FaLv], 1, &FaLBIdx, &MatchIdx );

#           ifdef DEBUG_HDF5
            if ( MatchIdx < 0 )
            Aux_Error( ERROR_INFO, "Lv %d, PID %d, FaPID %d, FaLBIdx %ld, couldn't find a matching patch !!\n",
                       lv, PID, FaPID, FaLBIdx );
#           endif

            FaGID = LBIdxList_Sort_IdxTable[FaLv][MatchIdx] + GID_LvStart[FaLv];
         } // if ( FaPID >= amr->NPatchComma[FaLv][1] )

         FaList_Local[lv][PID] = FaGID;


//       4-3-4. son GID
         SonPID = amr->patch[0][lv][PID]->son;
         SonLv  = lv + 1;

//       no son (must check this first since SonLv may be out of range --> == NLEVEL)
         if      ( SonPID == -1 )
            SonGID = SonPID;

//       son patch is a real patch at home
         else if ( SonPID >= 0  &&  SonPID < amr->NPatchComma[SonLv][1] )
            SonGID = SonPID + GID_Offset[SonLv];

//       son patch lives abroad (only possible in LOAD_BALANCE)
         else if ( SonPID < -1 )
         {
#           ifdef DEBUG_HDF5
#           ifdef LOAD_BALANCE
            const int SonRank = SON_OFFSET_LB - SonPID;
            if ( SonRank < 0  ||  SonRank == MPI_Rank  ||  SonRank >= MPI_NRank )
            Aux_Error( ERROR_INFO, "Lv %d, PID %d, SonPID %d, incorrect SonRank %d (MyRank %d, NRank %d) !!\n",
                       lv, PID, SonPID, SonRank, MPI_Rank, MPI_NRank );
#           else
            Aux_Error( ERROR_INFO, "Lv %d, PID %d, SonPID %d < -1 (only possible in LOAD_BALANCE) !!\n",
                       lv, PID, SonPID );
#           endif // LOAD_BALANCE
#           endif // DEBUG_HDF5

//          get the SonGID by "father corner = son corner -> son LB_Idx -> son GID"
//          --> didn't assume any relation between son's and father's LB_Idx
//          (although for Hilbert curve we have "SonLBIdx-SonLBIdx%8 = 8*MyLBIdx")
            SonCr    = amr->patch[0][lv][PID]->corner;
            SonLBIdx = LB_Corner2Index( SonLv, SonCr, CHECK_ON );

#           if ( defined DEBUG_HDF5  &&  LOAD_BALANCE == HILBERT )
            if ( SonLBIdx - SonLBIdx%8 != 8*amr->patch[0][lv][PID]->LB_Idx )
            Aux_Error( ERROR_INFO, "Lv %d, PID %d, SonPID %d, SonCr (%d,%d,%d), incorret SonLBIdx %ld, (MyLBIdx %ld) !!\n",
                       lv, PID, SonPID, SonCr[0], SonCr[1], SonCr[2], SonLBIdx, amr->patch[0][lv][PID]->LB_Idx );
#           endif

            Mis_Matching_int( NPatchTotal[SonLv], LBIdxList_Sort[SonLv], 1, &SonLBIdx, &MatchIdx );

#           ifdef DEBUG_HDF5
            if ( MatchIdx < 0 )
            Aux_Error( ERROR_INFO, "Lv %d, PID %d, SonPID %d, SonLBIdx %ld, couldn't find a matching patch !!\n",
                       lv, PID, SonPID, SonLBIdx );
#           endif

            SonGID = LBIdxList_Sort_IdxTable[SonLv][MatchIdx] + GID_LvStart[SonLv];
         } // else if ( SonPID < -1 )

//       son patch is a buffer patch (SonPID >= amr->NPatchComma[SonLv][1]) --> impossible
         else // ( SonPID >= amr->NPatchComma[SonLv][1] )
            Aux_Error( ERROR_INFO, "Lv %d, PID %d, SonPID %d is a buffer patch (NRealSonPatch %d) !!\n",
                       lv, PID, SonPID, amr->NPatchComma[SonLv][1] );

         SonList_Local[lv][PID] = SonGID;


//       4-3-5. sibling GID
         for (int s=0; s<26; s++)
         {
            SibPID = amr->patch[0][lv][PID]->sibling[s];

//          no sibling (SibPID can be either -1 or SIB_OFFSET_NONPERIODIC-BoundaryDirection)
            if      ( SibPID < 0 )
               SibGID = SibPID;

//          sibling patch is a real patch
            else if ( SibPID < amr->NPatchComma[lv][1] )
               SibGID = SibPID + GID_Offset[lv];

//          sibling patch is a buffer patch (which may lie outside the simulation domain)
            else
            {
#              ifdef DEBUG_HDF5
               if ( SibPID >= amr->num[lv] )
               Aux_Error( ERROR_INFO, "Lv %d, PID %d, SibPID %d >= total number of patches %d !!\n",
                          lv, PID, SibPID, amr->num[lv] );
#              endif

//             get the SibGID by "sibling corner -> sibling LB_Idx -> sibling GID"
               SibCr    = amr->patch[0][lv][SibPID]->corner;
               SibLBIdx = LB_Corner2Index( lv, SibCr, CHECK_OFF );   // periodicity has been assumed here 

               Mis_Matching_int( NPatchTotal[lv], LBIdxList_Sort[lv], 1, &SibLBIdx, &MatchIdx );

#              ifdef DEBUG_HDF5
               if ( MatchIdx < 0 )
               Aux_Error( ERROR_INFO, "Lv %d, PID %d, SibPID %d, SibLBIdx %ld, couldn't find a matching patch !!\n",
                          lv, PID, SibPID, SibLBIdx );
#              endif

               SibGID = LBIdxList_Sort_IdxTable[lv][MatchIdx] + GID_LvStart[lv];
            } // if ( SibPID >= amr->NPatchComma[lv][1] )

            SibList_Local[lv][PID][s] = SibGID;

         } // for (int s=0; s<26; s++)
      } // for (int PID=0; PID<amr->NPatchComma[lv][1]; PID++)
   } // for (int lv=0; lv<NLEVEL; lv++)


// 4-4. gather data from all ranks
   for (int lv=0; lv<NLEVEL; lv++)
   {
      for (int r=0; r<MPI_NRank; r++)
      {
         RecvCount_Fa [r] = NPatchAllRank[r][lv];
         RecvCount_Son[r] = RecvCount_Fa[r];
         RecvCount_Sib[r] = RecvCount_Fa[r]*26;
         RecvCount_Cr [r] = RecvCount_Fa[r]*3;

         RecvDisp_Fa  [r] = ( r == 0 ) ? 0 : RecvDisp_Fa[r-1] + RecvCount_Fa[r-1];
         RecvDisp_Son [r] = RecvDisp_Fa[r];
         RecvDisp_Sib [r] = RecvDisp_Fa[r]*26;
         RecvDisp_Cr  [r] = RecvDisp_Fa[r]*3;
      }

//    note that we collect data at one level at a time
      MPI_Gatherv( FaList_Local[lv],     amr->NPatchComma[lv][1],    MPI_INT,
                   FaList_AllLv+GID_LvStart[lv],       RecvCount_Fa,  RecvDisp_Fa,  MPI_INT, 0, MPI_COMM_WORLD );

      MPI_Gatherv( SonList_Local[lv],    amr->NPatchComma[lv][1],    MPI_INT,
                   SonList_AllLv+GID_LvStart[lv],      RecvCount_Son, RecvDisp_Son, MPI_INT, 0, MPI_COMM_WORLD );

      MPI_Gatherv( SibList_Local[lv][0], amr->NPatchComma[lv][1]*26, MPI_INT,
                   (SibList_AllLv+GID_LvStart[lv])[0], RecvCount_Sib, RecvDisp_Sib, MPI_INT, 0, MPI_COMM_WORLD );

      MPI_Gatherv( CrList_Local[lv][0],  amr->NPatchComma[lv][1]*3,  MPI_INT,
                   (CrList_AllLv+GID_LvStart[lv])[0],  RecvCount_Cr,  RecvDisp_Cr,  MPI_INT, 0, MPI_COMM_WORLD );
   } // for (int lv=0; lv<NLEVEL; lv++)


// 4-5. dump the tree info
   if ( MPI_Rank == 0 )
   {
//    reopen file
      H5_FileID = H5Fopen( FileName, H5F_ACC_RDWR, H5P_DEFAULT );
      if ( H5_FileID < 0 )    Aux_Error( ERROR_INFO, "failed to open the HDF5 file \"%s\" !!\n", FileName );

      H5_GroupID_Tree = H5Gcreate( H5_FileID, "Tree", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT );
      if ( H5_GroupID_Tree < 0 )    Aux_Error( ERROR_INFO, "failed to create the group \"%s\" !!\n", "Tree" );

//    4-5-1. LBIdx
      H5_SetDims_LBIdx = NPatchAllLv;
      H5_SpaceID_LBIdx = H5Screate_simple( 1, &H5_SetDims_LBIdx, NULL );
      H5_SetID_LBIdx   = H5Dcreate( H5_GroupID_Tree, "LBIdx", H5T_NATIVE_LONG, H5_SpaceID_LBIdx,
                                    H5P_DEFAULT, H5_DataCreatePropList, H5P_DEFAULT );

      if ( H5_SetID_LBIdx < 0 )  Aux_Error( ERROR_INFO, "failed to create the dataset \"%s\" !!\n", "LBIdx" );

      H5_Status = H5Dwrite( H5_SetID_LBIdx, H5T_NATIVE_LONG, H5S_ALL, H5S_ALL, H5P_DEFAULT, LBIdxList_AllLv );
      H5_Status = H5Dclose( H5_SetID_LBIdx );
      H5_Status = H5Sclose( H5_SpaceID_LBIdx );

//    4-5-2. corner
      H5_SetDims_Cr[0] = NPatchAllLv;
      H5_SetDims_Cr[1] = 3;
      H5_SpaceID_Cr    = H5Screate_simple( 2, H5_SetDims_Cr, NULL );
      H5_SetID_Cr      = H5Dcreate( H5_GroupID_Tree, "Corner", H5T_NATIVE_INT, H5_SpaceID_Cr,
                                    H5P_DEFAULT, H5_DataCreatePropList, H5P_DEFAULT );

      if ( H5_SetID_Cr < 0 )    Aux_Error( ERROR_INFO, "failed to create the dataset \"%s\" !!\n", "Corner" );

//    attach the attribute for converting corner to physical coordinates
      H5_AttID_Cvt2Phy = H5Acreate( H5_SetID_Cr, "Cvt2Phy", H5T_NATIVE_DOUBLE, H5_SpaceID_Scalar,
                                    H5P_DEFAULT, H5P_DEFAULT );

      if ( H5_AttID_Cvt2Phy < 0 )   Aux_Error( ERROR_INFO, "failed to create the attribute \"%s\" !!\n", "Cvt2Phy" );

      H5_Status = H5Awrite( H5_AttID_Cvt2Phy, H5T_NATIVE_DOUBLE, &amr->dh[TOP_LEVEL] );
      H5_Status = H5Aclose( H5_AttID_Cvt2Phy );

      H5_Status = H5Dwrite( H5_SetID_Cr, H5T_NATIVE_INT, H5S_ALL, H5S_ALL, H5P_DEFAULT, CrList_AllLv );
      H5_Status = H5Dclose( H5_SetID_Cr );
      H5_Status = H5Sclose( H5_SpaceID_Cr );

//    4-5-3. father
      H5_SetDims_Fa = NPatchAllLv;
      H5_SpaceID_Fa = H5Screate_simple( 1, &H5_SetDims_Fa, NULL );
      H5_SetID_Fa   = H5Dcreate( H5_GroupID_Tree, "Father", H5T_NATIVE_INT, H5_SpaceID_Fa,
                                 H5P_DEFAULT, H5_DataCreatePropList, H5P_DEFAULT );

      if ( H5_SetID_Fa < 0 )  Aux_Error( ERROR_INFO, "failed to create the dataset \"%s\" !!\n", "Father" );

      H5_Status = H5Dwrite( H5_SetID_Fa, H5T_NATIVE_INT, H5S_ALL, H5S_ALL, H5P_DEFAULT, FaList_AllLv );
      H5_Status = H5Dclose( H5_SetID_Fa );
      H5_Status = H5Sclose( H5_SpaceID_Fa );

//    4-5-4. son
      H5_SetDims_Son = NPatchAllLv;
      H5_SpaceID_Son = H5Screate_simple( 1, &H5_SetDims_Son, NULL );
      H5_SetID_Son   = H5Dcreate( H5_GroupID_Tree, "Son", H5T_NATIVE_INT, H5_SpaceID_Son,
                                  H5P_DEFAULT, H5_DataCreatePropList, H5P_DEFAULT );

      if ( H5_SetID_Son < 0 )  Aux_Error( ERROR_INFO, "failed to create the dataset \"%s\" !!\n", "Son" );

      H5_Status = H5Dwrite( H5_SetID_Son, H5T_NATIVE_INT, H5S_ALL, H5S_ALL, H5P_DEFAULT, SonList_AllLv );
      H5_Status = H5Dclose( H5_SetID_Son );
      H5_Status = H5Sclose( H5_SpaceID_Son );

//    4-5-5. sibling 
      H5_SetDims_Sib[0] = NPatchAllLv;
      H5_SetDims_Sib[1] = 26;
      H5_SpaceID_Sib    = H5Screate_simple( 2, H5_SetDims_Sib, NULL );
      H5_SetID_Sib      = H5Dcreate( H5_GroupID_Tree, "Sibling", H5T_NATIVE_INT, H5_SpaceID_Sib,
                                     H5P_DEFAULT, H5_DataCreatePropList, H5P_DEFAULT );

      if ( H5_SetID_Sib < 0 )    Aux_Error( ERROR_INFO, "failed to create the dataset \"%s\" !!\n", "Sibling" );

      H5_Status = H5Dwrite( H5_SetID_Sib, H5T_NATIVE_INT, H5S_ALL, H5S_ALL, H5P_DEFAULT, SibList_AllLv );
      H5_Status = H5Dclose( H5_SetID_Sib );
      H5_Status = H5Sclose( H5_SpaceID_Sib );

//    close file
      H5_Status = H5Gclose( H5_GroupID_Tree );
      H5_Status = H5Fclose( H5_FileID );
   } // if ( MPI_Rank == 0 )



// 5. output the simulation data (density, momentum, ... etc)
#  ifdef GRAVITY
   const int NOut = ( OPT__OUTPUT_POT ) ? NCOMP+1 : NCOMP;
#  else
   const int NOut = NCOMP;
#  endif
   const int FieldSizeOnePatch = sizeof(real)*CUBE(PS1);

   int  Sg;
   char FieldName[NOut][100];
   real (*FieldData)[PS1][PS1][PS1] = NULL;

// 5-1. set the output field names
#  if   ( MODEL == HYDRO )
   sprintf( FieldName[DENS], "Dens" );
   sprintf( FieldName[MOMX], "MomX" );
   sprintf( FieldName[MOMY], "MomY" );
   sprintf( FieldName[MOMZ], "MomZ" );
   sprintf( FieldName[ENGY], "Engy" );

#  elif ( MODEL == ELBDM )
   sprintf( FieldName[DENS], "Dens" );
   sprintf( FieldName[REAL], "Real" );
   sprintf( FieldName[IMAG], "Imag" );

#  else
#  error : ERROR : unsupported MODEL !!
#  endif

#  ifdef GRAVITY
   if ( OPT__OUTPUT_POT )
   sprintf( FieldName[NOut-1], "Pote" );
#  endif


// 5-2. initialize the "Data" group and datasets of all fields
   H5_SetDims_Field[0] = NPatchAllLv;
   H5_SetDims_Field[1] = PATCH_SIZE;
   H5_SetDims_Field[2] = PATCH_SIZE;
   H5_SetDims_Field[3] = PATCH_SIZE;

   H5_SpaceID_Field = H5Screate_simple( 4, H5_SetDims_Field, NULL );
   if ( H5_SpaceID_Field < 0 )   Aux_Error( ERROR_INFO, "failed to create the space \"%s\" !!\n", "H5_SpaceID_Field" );

   if ( MPI_Rank == 0 )
   {
//    HDF5 file must be synchronized before being written by the next rank
      SyncHDF5File( FileName );

      H5_FileID = H5Fopen( FileName, H5F_ACC_RDWR, H5P_DEFAULT );
      if ( H5_FileID < 0 )    Aux_Error( ERROR_INFO, "failed to open the HDF5 file \"%s\" !!\n", FileName );

//    create the "Data" group
      H5_GroupID_Data = H5Gcreate( H5_FileID, "Data", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT );
      if ( H5_GroupID_Data < 0 )    Aux_Error( ERROR_INFO, "failed to create the group \"%s\" !!\n", "Data" );

//    create the datasets of all fields
      for (int v=0; v<NOut; v++)
      {
         H5_SetID_Field = H5Dcreate( H5_GroupID_Data, FieldName[v], H5T_GAMER_REAL, H5_SpaceID_Field,
                                     H5P_DEFAULT, H5_DataCreatePropList, H5P_DEFAULT );
         if ( H5_SetID_Field < 0 )  Aux_Error( ERROR_INFO, "failed to create the dataset \"%s\" !!\n", FieldName[v] );
         H5_Status = H5Dclose( H5_SetID_Field );
      }

//    close the file and group
      H5_Status = H5Gclose( H5_GroupID_Data );
      H5_Status = H5Fclose( H5_FileID );
   } // if ( MPI_Rank == 0 )


// 5-3. start to dump data (serial instead of parallel)
   for (int lv=0; lv<NLEVEL; lv++)
   for (int TRank=0; TRank<MPI_NRank; TRank++)
   {
      if ( MPI_Rank == TRank )
      {
//       HDF5 file must be synchronized before being written by the next rank
         SyncHDF5File( FileName );

//       reopen the file and group
         H5_FileID = H5Fopen( FileName, H5F_ACC_RDWR, H5P_DEFAULT );
         if ( H5_FileID < 0 )    Aux_Error( ERROR_INFO, "failed to open the HDF5 file \"%s\" !!\n", FileName );

         H5_GroupID_Data = H5Gopen( H5_FileID, "Data", H5P_DEFAULT );
         if ( H5_GroupID_Data < 0 )    Aux_Error( ERROR_INFO, "failed to create the group \"%s\" !!\n", "Data" );


//       5-3-1. determine the memory space
         H5_MemDims_Field[0] = amr->NPatchComma[lv][1];
         H5_MemDims_Field[1] = PATCH_SIZE;
         H5_MemDims_Field[2] = PATCH_SIZE;
         H5_MemDims_Field[3] = PATCH_SIZE;

         H5_MemID_Field = H5Screate_simple( 4, H5_MemDims_Field, NULL );
         if ( H5_MemID_Field < 0 )  Aux_Error( ERROR_INFO, "failed to create the space \"%s\" !!\n", "H5_MemDims_Field" );


//       5-3-2. determine the subset of the dataspace
         H5_Offset_Field[0] = GID_Offset[lv];
         H5_Offset_Field[1] = 0;
         H5_Offset_Field[2] = 0;
         H5_Offset_Field[3] = 0;

         H5_Count_Field [0] = amr->NPatchComma[lv][1];
         H5_Count_Field [1] = PATCH_SIZE;
         H5_Count_Field [2] = PATCH_SIZE;
         H5_Count_Field [3] = PATCH_SIZE;

         H5_Status = H5Sselect_hyperslab( H5_SpaceID_Field, H5S_SELECT_SET, H5_Offset_Field, NULL, H5_Count_Field, NULL );
         if ( H5_Status < 0 )   Aux_Error( ERROR_INFO, "failed to create a hyperslab !!\n" );


//       output one field at one level in one rank at a time
         FieldData = new real [ amr->NPatchComma[lv][1] ][PS1][PS1][PS1];

         for (int v=0; v<NOut; v++)
         {
//          5-3-3. collect the target field from all patches at the current target level
#           ifdef GRAVITY
            Sg = ( v == NCOMP ) ? amr->PotSg[lv] : amr->FluSg[lv];
#           else
            Sg = amr->FluSg[lv];
#           endif

#           ifdef GRAVITY
            if ( v == NCOMP )
               for (int PID=0; PID<amr->NPatchComma[lv][1]; PID++)
                  memcpy( FieldData[PID], amr->patch[Sg][lv][PID]->pot,      FieldSizeOnePatch );
            else
#           endif
               for (int PID=0; PID<amr->NPatchComma[lv][1]; PID++)
                  memcpy( FieldData[PID], amr->patch[Sg][lv][PID]->fluid[v], FieldSizeOnePatch );


//          5-3-4. write data
            H5_SetID_Field = H5Dopen( H5_GroupID_Data, FieldName[v], H5P_DEFAULT );

            H5_Status = H5Dwrite( H5_SetID_Field, H5T_GAMER_REAL, H5_MemID_Field, H5_SpaceID_Field, H5P_DEFAULT, FieldData );
            if ( H5_Status < 0 )   Aux_Error( ERROR_INFO, "failed to write a field (lv %d, rank %d, v %d !!\n", lv, MPI_Rank, v );

            H5_Status = H5Dclose( H5_SetID_Field );
         } // for (int v=0; v<NOut; v++)

//       free resource
         delete [] FieldData;

         H5_Status = H5Sclose( H5_MemID_Field );
         H5_Status = H5Gclose( H5_GroupID_Data );
         H5_Status = H5Fclose( H5_FileID );
      } // if ( MPI_Rank == TRank )

      MPI_Barrier( MPI_COMM_WORLD );
   } // for (int TRank=0; TRank<MPI_NRank; TRank++) ... (int lv=0; lv<NLEVEL; lv++)

   H5_Status = H5Sclose( H5_SpaceID_Field );



// 6. check
#  ifdef DEBUG_HDF5
   if ( MPI_Rank == 0 )
   {
      const int MirrorSib[26] = { 1,0,3,2,5,4,9,8,7,6,13,12,11,10,17,16,15,14,25,24,23,22,21,20,19,18 };

      H5_FileID = H5Fopen( FileName, H5F_ACC_RDONLY, H5P_DEFAULT );
      if ( H5_FileID < 0 )    Aux_Error( ERROR_INFO, "failed to open the HDF5 file \"%s\" !!\n", FileName );

//    6-1. validate the father-son relation
//    6-1-1. load data
      char SetName[100];
      sprintf( SetName, "Tree/Father" );
      H5_SetID_Fa = H5Dopen( H5_FileID, SetName, H5P_DEFAULT );

      if ( H5_SetID_Fa < 0 )  Aux_Error( ERROR_INFO, "failed to open the dataset \"%s\" !!\n", SetName );

      H5_Status = H5Dread( H5_SetID_Fa, H5T_NATIVE_INT, H5S_ALL, H5S_ALL, H5P_DEFAULT, FaList_AllLv );
      H5_Status = H5Dclose( H5_SetID_Fa );

      sprintf( SetName, "Tree/Son" );
      H5_SetID_Son = H5Dopen( H5_FileID, SetName, H5P_DEFAULT );

      if ( H5_SetID_Son < 0 )  Aux_Error( ERROR_INFO, "failed to open the dataset \"%s\" !!\n", SetName );

      H5_Status = H5Dread( H5_SetID_Son, H5T_NATIVE_INT, H5S_ALL, H5S_ALL, H5P_DEFAULT, SonList_AllLv );
      H5_Status = H5Dclose( H5_SetID_Son );

      sprintf( SetName, "Tree/Sibling" );
      H5_SetID_Sib = H5Dopen( H5_FileID, SetName, H5P_DEFAULT );

      if ( H5_SetID_Sib < 0 )  Aux_Error( ERROR_INFO, "failed to open the dataset \"%s\" !!\n", SetName );

      H5_Status = H5Dread( H5_SetID_Sib, H5T_NATIVE_INT, H5S_ALL, H5S_ALL, H5P_DEFAULT, SibList_AllLv );
      H5_Status = H5Dclose( H5_SetID_Sib );


      for (int lv=0; lv<NLEVEL; lv++)
      for (int GID=GID_LvStart[lv]; GID<GID_LvStart[lv]+NPatchTotal[lv]; GID++)
      {
//       6-1-2. root patches have no father
         if ( lv == 0 )
         if ( FaList_AllLv[GID] != -1 )
            Aux_Error( ERROR_INFO, "Lv %d, GID %d, FaGID %d != -1 !!\n", lv, GID, FaList_AllLv[GID] );

//       6-1-3. all patches at refinement levels have fathers
         if ( lv > 0 )
         if ( FaList_AllLv[GID] < 0  ||  FaList_AllLv[GID] >= GID_LvStart[lv] )
            Aux_Error( ERROR_INFO, "Lv %d, GID %d, FaGID %d < 0 (or > max = %d) !!\n",
                       lv, GID, FaList_AllLv[GID], GID_LvStart[lv]-1 );

//       6-1-4. father->son == itself
         if ( lv > 0 )
         if ( SonList_AllLv[ FaList_AllLv[GID] ] + GID%8 != GID )
            Aux_Error( ERROR_INFO, "Lv %d, GID %d, FaGID %d, FaGID->Son %d ==> inconsistent !!\n",
                       lv, GID, FaList_AllLv[GID], SonList_AllLv[ FaList_AllLv[GID] ] );

//       6-1-5. son->father == itself
         SonGID = SonList_AllLv[GID];
         if ( SonGID != -1 )
         {
            if ( lv >= MAX_LEVEL )
               Aux_Error( ERROR_INFO, "Lv %d, GID %d, SonGID %d != -1 !!\n", lv, GID, SonGID );

            if ( SonGID < -1 )
               Aux_Error( ERROR_INFO, "Lv %d, GID %d, SonGID %d < -1 !!\n", lv, GID, SonGID );

            if ( lv < NLEVEL-1  &&  SonGID >= GID_LvStart[lv+1]+NPatchTotal[lv+1] )
               Aux_Error( ERROR_INFO, "Lv %d, GID %d, SonGID %d > max %d !!\n", lv, GID, SonGID,
                          GID_LvStart[lv+1]+NPatchTotal[lv+1]-1 );

            for (int LocalID=0; LocalID<8; LocalID++)
            if ( FaList_AllLv[SonGID+LocalID] != GID )
               Aux_Error( ERROR_INFO, "Lv %d, GID %d, SonGID %d, SonGID->Father %d ==> inconsistent !!\n",
                          lv, GID, SonGID+LocalID, FaList_AllLv[SonGID+LocalID] );
         }

//       6-1-6. sibling->sibling_mirror = itself
         for (int s=0; s<26; s++)
         {
            SibGID = SibList_AllLv[GID][s];

            if ( SibGID >= 0 )
            {
               if ( SibGID < GID_LvStart[lv]  ||  SibGID >= GID_LvStart[lv]+NPatchTotal[lv] )
                  Aux_Error( ERROR_INFO, "Lv %d, GID %d, sib %d, SibGID %d lies outside the correct range (%d <= SibGID < %d) !!\n",
                             lv, GID, s, SibGID, GID_LvStart[lv], GID_LvStart[lv]+NPatchTotal[lv] );

               if ( SibList_AllLv[SibGID][ MirrorSib[s] ] != GID )
                  Aux_Error( ERROR_INFO, "Lv %d, GID %d, sib %d, SibGID %d != SibGID->sibling %d !!\n", 
                             lv, GID, s, SibGID, SibList_AllLv[SibGID][ MirrorSib[s] ] );
            }
         }
      } // for (int lv=0; lv<NLEVEL; lv++)

      H5_Status = H5Fclose( H5_FileID );
   } // if ( MPI_Rank == 0 )
#  endif // #ifdef DEBUG_HDF5



// 7. close all HDF5 objects and free memory
   H5_Status = H5Tclose( H5_TypeID_Com_KeyInfo );
   H5_Status = H5Tclose( H5_TypeID_Com_Makefile );
   H5_Status = H5Tclose( H5_TypeID_Com_SymConst );
   H5_Status = H5Tclose( H5_TypeID_Com_InputPara );
   H5_Status = H5Sclose( H5_SpaceID_Scalar );
   H5_Status = H5Pclose( H5_DataCreatePropList );

   delete [] NPatchAllRank;

   if ( MPI_Rank == 0 )
   {
      delete [] LBIdxList_AllLv;
      delete []    CrList_AllLv;
      delete []    FaList_AllLv;
      delete []   SonList_AllLv;
      delete []   SibList_AllLv;
   }

   for (int lv=0; lv<NLEVEL; lv++)
   {
      delete [] LBIdxList_Local[lv];
      delete []    CrList_Local[lv];
      delete []    FaList_Local[lv];
      delete []   SonList_Local[lv];
      delete []   SibList_Local[lv];

      delete [] LBIdxList_Sort[lv];
      delete [] LBIdxList_Sort_IdxTable[lv];
   }


   if ( MPI_Rank == 0 )    Aux_Message( stdout, "%s (DumpID = %d) ... done\n", __FUNCTION__, DumpID );

} // FUNCTION : Output_DumpData_Total_HDF5



//-------------------------------------------------------------------------------------------------------
// Function    :  FillIn_KeyInfo
// Description :  Fill in the KeyInfo_t structure 
//
// Note        :  1. Data sturcture is defined in "HDF5_Typedef.h"
//                2. Call-by-reference
//
// Parameter   :  KeyInfo  : Pointer to be filled in
//-------------------------------------------------------------------------------------------------------
void FillIn_KeyInfo( KeyInfo_t &KeyInfo )
{

   const time_t CalTime       = time( NULL );   // calendar time

   KeyInfo.FormatVersion      = 2101;
   KeyInfo.Model              = MODEL;
   KeyInfo.NLevel             = NLEVEL;
   KeyInfo.PatchSize          = PATCH_SIZE;
   KeyInfo.DumpID             = DumpID;
   KeyInfo.Step               = Step;
#  ifdef GRAVITY
   KeyInfo.OutputPot          = (OPT__OUTPUT_POT) ? 1 : 0;
   KeyInfo.AveDens            = AveDensity;
   KeyInfo.Gravity            = 1;
#  else
   KeyInfo.Gravity            = 0;
#  endif
#  ifdef PARTICLE
   KeyInfo.Particle           = 1;
#  else
   KeyInfo.Particle           = 0;
#  endif
#  ifdef FLOAT8
   KeyInfo.Float8             = 1;
#  else
   KeyInfo.Float8             = 0;
#  endif

   for (int d=0; d<3; d++)
   {
      KeyInfo.NX0     [d] = NX0_TOT      [d];
      KeyInfo.BoxScale[d] = amr->BoxScale[d];
      KeyInfo.BoxSize [d] = amr->BoxSize [d];
   }

   for (int lv=0; lv<NLEVEL; lv++)
   {
      KeyInfo.Time          [lv] = Time          [lv];
      KeyInfo.CellSize      [lv] = amr->dh       [lv];
      KeyInfo.CellScale     [lv] = amr->scale    [lv];
      KeyInfo.NPatch        [lv] = NPatchTotal   [lv];
      KeyInfo.AdvanceCounter[lv] = AdvanceCounter[lv];
   }

   KeyInfo.CodeVersion  = "GAMER.1.0.beta5.4.0.t94-22";
   KeyInfo.DumpWallTime = ctime( &CalTime );
   KeyInfo.DumpWallTime[ strlen(KeyInfo.DumpWallTime)-1 ] = '\0';  // remove the last character '\n'

} // FUNCTION : FillIn_KeyInfo



//-------------------------------------------------------------------------------------------------------
// Function    :  FillIn_Makefile
// Description :  Fill in the Makefile_t structure 
//
// Note        :  1. Data sturcture is defined in "HDF5_Typedef.h"
//                2. Call-by-reference
//
// Parameter   :  Makefile : Pointer to be filled in
//-------------------------------------------------------------------------------------------------------
void FillIn_Makefile( Makefile_t &Makefile )
{

// model-independent options
   Makefile.Model              = MODEL;

#  ifdef GRAVITY
   Makefile.Gravity            = 1;
#  else
   Makefile.Gravity            = 0;
#  endif

#  ifdef INDIVIDUAL_TIMESTEP
   Makefile.IndividualDt       = 1;
#  else
   Makefile.IndividualDt       = 0;
#  endif

#  ifdef COMOVING
   Makefile.Comoving           = 1;
#  else
   Makefile.Comoving           = 0;
#  endif

#  ifdef PARTICLE
   Makefile.Particle           = 1;
#  else
   Makefile.Particle           = 0;
#  endif


#  ifdef GPU
   Makefile.UseGPU             = 1;
#  else
   Makefile.UseGPU             = 0;
#  endif

#  ifdef GAMER_OPTIMIZATION
   Makefile.GAMER_Optimization = 1;
#  else
   Makefile.GAMER_Optimization = 0;
#  endif

#  ifdef GAMER_DEBUG
   Makefile.GAMER_Debug        = 1;
#  else
   Makefile.GAMER_Debug        = 0;
#  endif

#  ifdef TIMING
   Makefile.Timing             = 1;
#  else
   Makefile.Timing             = 0;
#  endif

#  ifdef TIMING_SOLVER
   Makefile.TimingSolver       = 1;
#  else
   Makefile.TimingSolver       = 0;
#  endif

#  ifdef INTEL
   Makefile.Intel              = 1;
#  else
   Makefile.Intel              = 0;
#  endif

#  ifdef FLOAT8
   Makefile.Float8             = 1;
#  else
   Makefile.Float8             = 0;
#  endif

#  ifdef SERIAL
   Makefile.Serial             = 1;
#  else
   Makefile.Serial             = 0;
#  endif

#  ifdef LOAD_BALANCE
   Makefile.LoadBalance        = LOAD_BALANCE;
#  else
   Makefile.LoadBalance        = 0;
#  endif

#  ifdef OVERLAP_MPI
   Makefile.OverlapMPI         = 1;
#  else
   Makefile.OverlapMPI         = 0;
#  endif

#  ifdef OPENMP
   Makefile.OpenMP             = 1;
#  else
   Makefile.OpenMP             = 0;
#  endif

#  ifdef GPU
   Makefile.GPU_Arch           = GPU_ARCH;
#  else
   Makefile.GPU_Arch           = NULL_INT;
#  endif

#  ifdef LAOHU
   Makefile.Laohu              = 1;
#  else
   Makefile.Laohu              = 0;
#  endif

#  ifdef SUPPORT_HDF5
   Makefile.SupportHDF5        = 1;
#  else
   Makefile.SupportHDF5        = 0;
#  endif

   Makefile.NLevel             = NLEVEL;
   Makefile.MaxPatch           = MAX_PATCH;


// model-dependent options
#  ifdef GRAVITY
   Makefile.PotScheme          = POT_SCHEME;
#  ifdef STORE_POT_GHOST
   Makefile.StorePotGhost      = 1;
#  else
   Makefile.StorePotGhost      = 0;
#  endif
#  ifdef UNSPLIT_GRAVITY
   Makefile.UnsplitGravity     = 1;
#  else
   Makefile.UnsplitGravity     = 0;
#  endif
#  endif

#  if   ( MODEL == HYDRO )
   Makefile.FluScheme          = FLU_SCHEME;
#  ifdef LR_SCHEME
   Makefile.LRScheme           = LR_SCHEME;
#  endif
#  ifdef RSOLVER
   Makefile.RSolver            = RSOLVER;
#  endif
   Makefile.NPassive           = NPASSIVE;

#  elif ( MODEL == MHD )
#  warning : WAIT MHD !!!

#  elif ( MODEL == ELBDM )
#  ifdef CONSERVE_MASS
   Makefile.ConserveMass       = 1;
#  else
   Makefile.ConserveMass       = 0;
#  endif

#  ifdef LAPLACIAN_4TH
   Makefile.Laplacian4th       = 1;
#  else
   Makefile.Laplacian4th       = 0;
#  endif

#  ifdef QUARTIC_SELF_INTERACTION
   Makefile.SelfInteraction4   = 1;
#  else
   Makefile.SelfInteraction4   = 0;
#  endif

#  else
#  error : unsupported MODEL !!
#  endif // MODEL

} // FUNCTION : FillIn_Makefile



//-------------------------------------------------------------------------------------------------------
// Function    :  FillIn_SymConst
// Description :  Fill in the SymConst_t structure 
//
// Note        :  1. Data sturcture is defined in "HDF5_Typedef.h"
//                2. Call-by-reference
//
// Parameter   :  SymConst : Pointer to be filled in
//-------------------------------------------------------------------------------------------------------
void FillIn_SymConst( SymConst_t &SymConst )
{

// model-independent variables
   SymConst.NComp                = NCOMP;
   SymConst.PatchSize            = PATCH_SIZE;
   SymConst.Flu_NIn              = FLU_NIN;
   SymConst.Flu_NOut             = FLU_NOUT;
   SymConst.NFlux                = NFLUX;
   SymConst.Flu_GhostSize        = FLU_GHOST_SIZE;
   SymConst.Flu_Nxt              = FLU_NXT;
#  ifdef DEBUG_HDF5
   SymConst.Debug_HDF5           = 1;
#  else
   SymConst.Debug_HDF5           = 0;
#  endif
   SymConst.SibOffsetNonperiodic = SIB_OFFSET_NONPERIODIC;
#  ifdef LOAD_BALANCE
   SymConst.SonOffsetLB          = SON_OFFSET_LB;
#  endif
   SymConst.TinyValue            = TINY_VALUE;


// model-dependent variables
#  ifdef GRAVITY
   SymConst.Gra_NIn              = GRA_NIN;
   SymConst.Pot_GhostSize        = POT_GHOST_SIZE;
   SymConst.Gra_GhostSize        = GRA_GHOST_SIZE;
   SymConst.Rho_GhostSize        = RHO_GHOST_SIZE;
   SymConst.Pot_Nxt              = POT_NXT;
   SymConst.Gra_Nxt              = GRA_NXT;
   SymConst.Rho_Nxt              = RHO_NXT;

#  ifdef UNSPLIT_GRAVITY
   SymConst.USG_GhostSize        = USG_GHOST_SIZE;
   SymConst.USG_NxtF             = USG_NXT_F;
   SymConst.USG_NxtG             = USG_NXT_G;
#  endif

   SymConst.Gra_BlockSize_z      = GRA_BLOCK_SIZE_Z;
   SymConst.ExtPotNAuxMax        = EXT_POT_NAUX_MAX;
   SymConst.ExtAccNAuxMax        = EXT_ACC_NAUX_MAX;

#  if   ( POT_SCHEME == SOR )
   SymConst.Pot_BlockSize_z      = POT_BLOCK_SIZE_Z;
#  ifdef USE_PSOLVER_10TO14
   SymConst.UsePSolver_10to14    = 1;
#  else
   SymConst.UsePSolver_10to14    = 0;
#  endif
#  elif ( POT_SCHEME == MG  )
   SymConst.Pot_BlockSize_x      = POT_BLOCK_SIZE_X;
#  endif
#  endif // #ifdef GRAVITY


#  ifdef PARTICLE
   SymConst.NPar_Var             = NPAR_VAR;
   SymConst.NPar_Passive         = NPAR_PASSIVE;

#  ifdef DEBUG_PARTICLE
   SymConst.Debug_Particle       = 1;
#  else
   SymConst.Debug_Particle       = 0;
#  endif

   SymConst.ParList_GrowthFactor = PARLIST_GROWTH_FACTOR;
   SymConst.ParList_ReduceFactor = PARLIST_REDUCE_FACTOR;
#  endif


#  if   ( MODEL == HYDRO )
   SymConst.Flu_BlockSize_x      = FLU_BLOCK_SIZE_X;
   SymConst.Flu_BlockSize_y      = FLU_BLOCK_SIZE_Y;
#  ifdef CHECK_NEGATIVE_IN_FLUID
   SymConst.CheckNegativeInFluid = 1;
#  else
   SymConst.CheckNegativeInFluid = 0;
#  endif
#  ifdef CHAR_RECONSTRUCTION
   SymConst.CharReconstruction   = 1;
#  else
   SymConst.CharReconstruction   = 0;
#  endif
#  ifdef CHECK_INTERMEDIATE
   SymConst.CheckIntermediate    = CHECK_INTERMEDIATE;
#  else
   SymConst.CheckIntermediate    = 0;
#  endif
#  ifdef HLL_NO_REF_STATE
   SymConst.HLL_NoRefState       = 1;
#  else
   SymConst.HLL_NoRefState       = 0;
#  endif
#  ifdef HLL_INCLUDE_ALL_WAVES
   SymConst.HLL_IncludeAllWaves  = 1;
#  else
   SymConst.HLL_IncludeAllWaves  = 0;
#  endif
#  ifdef WAF_DISSIPATE
   SymConst.WAF_Dissipate        = 1;
#  else
   SymConst.WAF_Dissipate        = 0;
#  endif
#  ifdef POSITIVE_DENS_IN_FIXUP
   SymConst.PositiveDensInFixUp  = 1;
#  else
   SymConst.PositiveDensInFixUp  = 0;
#  endif

#  ifdef N_FC_VAR
   SymConst.N_FC_Var             = N_FC_VAR;
#  endif

#  ifdef N_SLOPE_PPM
   SymConst.N_Slope_PPM          = N_SLOPE_PPM;
#  endif

#  ifdef MIN_PRES_DENS
   SymConst.Min_Pres_Dens        = MIN_PRES_DENS;
#  endif

#  ifdef MIN_PRES
   SymConst.Min_Pres             = MIN_PRES;
#  endif

#  ifdef MAX_ERROR
   SymConst.MaxError             = MAX_ERROR;
#  endif


#  elif ( MODEL == MHD )
   SymConst.Flu_BlockSize_x      = FLU_BLOCK_SIZE_X;
   SymConst.Flu_BlockSize_y      = FLU_BLOCK_SIZE_Y;
#  warning : WAIT MHD !!!


#  elif  ( MODEL == ELBDM )
   SymConst.Flu_BlockSize_x      = FLU_BLOCK_SIZE_X;
   SymConst.Flu_BlockSize_y      = FLU_BLOCK_SIZE_Y;

#  else
#  error : ERROR : unsupported MODEL !!
#  endif // MODEL

} // FUNCTION : FillIn_SymConst



//-------------------------------------------------------------------------------------------------------
// Function    :  FillIn_InputPara
// Description :  Fill in the InputPara_t structure 
//
// Note        :  1. Data sturcture is defined in "HDF5_Typedef.h"
//                2. Call-by-reference
//
// Parameter   :  InputPara : Pointer to be filled in
//-------------------------------------------------------------------------------------------------------
void FillIn_InputPara( InputPara_t &InputPara )
{

// simulation scale
   InputPara.BoxSize                 = BOX_SIZE;
   for (int d=0; d<3; d++)
   InputPara.NX0_Tot[d]              = NX0_TOT[d];
   InputPara.MPI_NRank               = MPI_NRank;
   for (int d=0; d<3; d++)
   InputPara.MPI_NRank_X[d]          = MPI_NRank_X[d];
   InputPara.OMP_NThread             = OMP_NTHREAD;
   InputPara.EndT                    = END_T;
   InputPara.EndStep                 = END_STEP;

// boundary condition
   for (int t=0; t<6; t++)
   InputPara.Opt__BC_Flu[t]          = OPT__BC_FLU[t];
#  ifdef GRAVITY
   InputPara.Opt__BC_Pot             = OPT__BC_POT;
   InputPara.GFunc_Coeff0            = GFUNC_COEFF0;
#  endif

// particle
#  ifdef PARTICLE
   InputPara.Par_NPar                = amr->Par->NPar;
   InputPara.Par_Init                = amr->Par->Init;
   InputPara.Par_Interp              = amr->Par->Interp;
   InputPara.Par_Integ               = amr->Par->Integ;
   InputPara.Par_ImproveAcc          = amr->Par->ImproveAcc;
   InputPara.Par_PredictPos          = amr->Par->PredictPos;
   InputPara.Par_RemoveCell          = amr->Par->RemoveCell;
#  endif

// cosmology
#  ifdef COMOVING
   InputPara.A_Init                  = A_INIT;
   InputPara.OmegaM0                 = OMEGA_M0;
#  endif

// time-step determination
   InputPara.Dt__Fluid               = DT__FLUID;
   InputPara.Dt__FluidInit           = DT__FLUID_INIT;
#  ifdef GRAVITY
   InputPara.Dt__Gravity             = DT__GRAVITY;
#  endif
#  if ( MODEL == ELBDM )
   InputPara.Dt__Phase               = DT__PHASE;
#  endif
#  ifdef PARTICLE 
   InputPara.Dt__ParVel              = DT__PARVEL;
   InputPara.Dt__ParVelMax           = DT__PARVEL_MAX;
#  endif
#  ifdef COMOVING
   InputPara.Dt__MaxDeltaA           = DT__MAX_DELTA_A;
#  endif
   InputPara.Opt__AdaptiveDt         = OPT__ADAPTIVE_DT;
   InputPara.Opt__RecordDt           = OPT__RECORD_DT;
   InputPara.Opt__DtUser             = OPT__DT_USER;
   
// domain refinement
   InputPara.RegridCount             = REGRID_COUNT;
   InputPara.FlagBufferSize          = FLAG_BUFFER_SIZE;
   InputPara.MaxLevel                = MAX_LEVEL;
   InputPara.Opt__Flag_Rho           = OPT__FLAG_RHO;
   InputPara.Opt__Flag_RhoGradient   = OPT__FLAG_RHO_GRADIENT;
#  if ( MODEL == HYDRO ) 
   InputPara.Opt__Flag_PresGradient  = OPT__FLAG_PRES_GRADIENT;
#  endif
#  if ( MODEL == ELBDM ) 
   InputPara.Opt__Flag_EngyDensity   = OPT__FLAG_ENGY_DENSITY;
#  endif
   InputPara.Opt__Flag_LohnerDens    = OPT__FLAG_LOHNER_DENS;
#  if ( MODEL == HYDRO ) 
   InputPara.Opt__Flag_LohnerEngy    = OPT__FLAG_LOHNER_ENGY;
   InputPara.Opt__Flag_LohnerPres    = OPT__FLAG_LOHNER_PRES;
#  endif
   InputPara.Opt__Flag_LohnerForm    = OPT__FLAG_LOHNER_FORM;
   InputPara.Opt__Flag_User          = OPT__FLAG_USER;
   InputPara.Opt__Flag_Region        = OPT__FLAG_REGION;
   InputPara.Opt__PatchCount         = OPT__PATCH_COUNT;
#  ifdef PARTICLE
   InputPara.Opt__ParLevel           = OPT__PAR_LEVEL;
#  endif

// load balance
#  ifdef LOAD_BALANCE
   InputPara.LB_Input__WLI_Max       = LB_INPUT__WLI_MAX;
#  endif

// fluid solvers in HYDRO
#  if ( MODEL == HYDRO )
   InputPara.Gamma                   = GAMMA;
   InputPara.MinMod_Coeff            = MINMOD_COEFF;
   InputPara.EP_Coeff                = EP_COEFF;
   InputPara.Opt__LR_Limiter         = OPT__LR_LIMITER;
   InputPara.Opt__WAF_Limiter        = OPT__WAF_LIMITER;
   InputPara.Opt__CorrUnphyScheme    = OPT__CORR_UNPHY_SCHEME;
#  endif

// ELBDM solvers
#  if ( MODEL == ELBDM )
   InputPara.ELBDM_Mass              = ELBDM_MASS;
   InputPara.ELBDM_PlanckConst       = ELBDM_PLANCK_CONST;
#  ifdef QUARTIC_SELF_INTERACTION
   InputPara.ELBDM_Lambda            = ELBDM_LAMBDA;
#  endif
   InputPara.ELBDM_Taylor3_Coeff     = ELBDM_TAYLOR3_COEFF;
   InputPara.ELBDM_Taylor3_Auto      = ELBDM_TAYLOR3_AUTO;
#  endif

// fluid solvers in both HYDRO/MHD/ELBDM
   InputPara.Flu_GPU_NPGroup         = FLU_GPU_NPGROUP;
   InputPara.GPU_NStream             = GPU_NSTREAM;
   InputPara.Opt__FixUp_Flux         = OPT__FIXUP_FLUX;
   InputPara.Opt__FixUp_Restrict     = OPT__FIXUP_RESTRICT;
   InputPara.Opt__OverlapMPI         = OPT__OVERLAP_MPI;
   InputPara.Opt__ResetFluid         = OPT__RESET_FLUID;
   InputPara.Opt__CorrUnphy          = OPT__CORR_UNPHY;

// self-gravity
#  ifdef GRAVITY
   InputPara.NewtonG                 = NEWTON_G;
#  if   ( POT_SCHEME == SOR )
   InputPara.SOR_Omega               = SOR_OMEGA;
   InputPara.SOR_MaxIter             = SOR_MAX_ITER;
   InputPara.SOR_MinIter             = SOR_MIN_ITER;
#  elif ( POT_SCHEME == MG )
   InputPara.MG_MaxIter              = MG_MAX_ITER;
   InputPara.MG_NPreSmooth           = MG_NPRE_SMOOTH;
   InputPara.MG_NPostSmooth          = MG_NPOST_SMOOTH;
   InputPara.MG_ToleratedError       = MG_TOLERATED_ERROR;
#  endif
   InputPara.Pot_GPU_NPGroup         = POT_GPU_NPGROUP;
   InputPara.Opt__GraP5Gradient      = OPT__GRA_P5_GRADIENT;
   InputPara.Opt__GravityType        = OPT__GRAVITY_TYPE;
   InputPara.Opt__ExternalPot        = OPT__EXTERNAL_POT;
#  endif

// initialization
   InputPara.Opt__Init               = OPT__INIT;
   InputPara.Opt__RestartHeader      = OPT__RESTART_HEADER;
   InputPara.Opt__UM_Start_Level     = OPT__UM_START_LEVEL;
   InputPara.Opt__UM_Start_NVar      = OPT__UM_START_NVAR;
   InputPara.Opt__UM_Start_Downgrade = OPT__UM_START_DOWNGRADE;
   InputPara.Opt__UM_Start_Refine    = OPT__UM_START_REFINE;
   InputPara.Opt__UM_Factor_5over3   = OPT__UM_FACTOR_5OVER3;
   InputPara.Opt__InitRestrict       = OPT__INIT_RESTRICT;
   InputPara.Opt__GPUID_Select       = OPT__GPUID_SELECT;
   InputPara.Init_Subsampling_NCell  = INIT_SUBSAMPLING_NCELL;

// interpolation schemes
   InputPara.Opt__Int_Time           = OPT__INT_TIME;
#  if ( MODEL == ELBDM ) 
   InputPara.Opt__Int_Phase          = OPT__INT_PHASE;
#  endif
   InputPara.Opt__Flu_IntScheme      = OPT__FLU_INT_SCHEME;
#  ifdef GRAVITY
   InputPara.Opt__Pot_IntScheme      = OPT__POT_INT_SCHEME;
   InputPara.Opt__Rho_IntScheme      = OPT__RHO_INT_SCHEME;
   InputPara.Opt__Gra_IntScheme      = OPT__GRA_INT_SCHEME;
#  endif
   InputPara.Opt__RefFlu_IntScheme   = OPT__REF_FLU_INT_SCHEME;
#  ifdef GRAVITY
   InputPara.Opt__RefPot_IntScheme   = OPT__REF_POT_INT_SCHEME;
#  endif
   InputPara.IntMonoCoeff            = INT_MONO_COEFF;

// data dump
   InputPara.Opt__Output_Total       = OPT__OUTPUT_TOTAL;
   InputPara.Opt__Output_Part        = OPT__OUTPUT_PART;
   InputPara.Opt__Output_TestError   = OPT__OUTPUT_TEST_ERROR;
#  ifdef PARTICLE
   InputPara.Opt__Output_Particle    = OPT__OUTPUT_PARTICLE;
#  endif
   InputPara.Opt__Output_BasePS      = OPT__OUTPUT_BASEPS;
   InputPara.Opt__Output_Base        = OPT__OUTPUT_BASE;
#  ifdef GRAVITY
   InputPara.Opt__Output_Pot         = OPT__OUTPUT_POT;
#  endif
   InputPara.Opt__Output_Mode        = OPT__OUTPUT_MODE;
   InputPara.Opt__Output_Step        = OUTPUT_STEP;
   InputPara.Opt__Output_Dt          = OUTPUT_DT;
   InputPara.Output_PartX            = OUTPUT_PART_X;
   InputPara.Output_PartY            = OUTPUT_PART_Y;
   InputPara.Output_PartZ            = OUTPUT_PART_Z;
   InputPara.InitDumpID              = INIT_DUMPID;

// miscellaneous
   InputPara.Opt__Verbose            = OPT__VERBOSE;
   InputPara.Opt__TimingBalance      = OPT__TIMING_BALANCE;
   InputPara.Opt__TimingMPI          = OPT__TIMING_MPI;
   InputPara.Opt__RecordMemory       = OPT__RECORD_MEMORY;
   InputPara.Opt__RecordPerformance  = OPT__RECORD_PERFORMANCE;
   InputPara.Opt__ManualControl      = OPT__MANUAL_CONTROL;
   InputPara.Opt__RecordUser         = OPT__RECORD_USER;

// simulation checks
   InputPara.Opt__Ck_Refine          = OPT__CK_REFINE;
   InputPara.Opt__Ck_ProperNesting   = OPT__CK_PROPER_NESTING;
   InputPara.Opt__Ck_Conservation    = OPT__CK_CONSERVATION;
   InputPara.Opt__Ck_Restrict        = OPT__CK_RESTRICT;
   InputPara.Opt__Ck_Finite          = OPT__CK_FINITE;
   InputPara.Opt__Ck_PatchAllocate   = OPT__CK_PATCH_ALLOCATE;
   InputPara.Opt__Ck_FluxAllocate    = OPT__CK_FLUX_ALLOCATE;
#  if ( MODEL == HYDRO )
   InputPara.Opt__Ck_Negative        = OPT__CK_NEGATIVE;
#  endif
   InputPara.Opt__Ck_MemFree         = OPT__CK_MEMFREE;
#  ifdef PARTICLE
   InputPara.Opt__Ck_Particle        = OPT__CK_PARTICLE;
#  endif

// flag tables
#  if   ( MODEL == HYDRO  ||  MODEL == MHD )
   const bool Opt__FlagLohner = ( OPT__FLAG_LOHNER_DENS || OPT__FLAG_LOHNER_ENGY || OPT__FLAG_LOHNER_PRES );
#  elif ( MODEL == ELBDM )
   const bool Opt__FlagLohner = OPT__FLAG_LOHNER_DENS;
#  endif

   for (int lv=0; lv<NLEVEL-1; lv++)
   {
      InputPara.FlagTable_Rho         [lv]    = FlagTable_Rho         [lv];
      InputPara.FlagTable_RhoGradient [lv]    = FlagTable_RhoGradient [lv];

      for (int t=0; t<3; t++)
      InputPara.FlagTable_Lohner      [lv][t] = FlagTable_Lohner      [lv][t];

      InputPara.FlagTable_User        [lv]    = FlagTable_User        [lv]; 

#     if   ( MODEL == HYDRO )
      InputPara.FlagTable_PresGradient[lv]    = FlagTable_PresGradient[lv];

#     elif ( MODEL == ELBDM )
      for (int t=0; t<2; t++)
      InputPara.FlagTable_EngyDensity [lv][t] = FlagTable_EngyDensity [lv][t];
#     endif
   }

} // FUNCTION : FillIn_InputPara



//-------------------------------------------------------------------------------------------------------
// Function    :  GetCompound_KeyInfo
// Description :  Create the HDF5 compound datatype for KeyInfo
//
// Note        :  1. Data sturcture is defined in "HDF5_Typedef.h"
//                2. The returned H5_TypeID must be closed manually
//                3. Call-by-reference
//
// Parameter   :  H5_TypeID   : HDF5 type ID for storing the compound datatype
//-------------------------------------------------------------------------------------------------------
void GetCompound_KeyInfo( hid_t &H5_TypeID )
{

// create the array type
   const hsize_t H5_ArrDims_3Var         = 3;                        // array size of [3]
   const hsize_t H5_ArrDims_NLv          = NLEVEL;                   // array size of [NLEVEL]

   const hid_t   H5_TypeID_Arr_3Double   = H5Tarray_create( H5T_NATIVE_DOUBLE, 1, &H5_ArrDims_3Var      );
   const hid_t   H5_TypeID_Arr_3Int      = H5Tarray_create( H5T_NATIVE_INT,    1, &H5_ArrDims_3Var      );
   const hid_t   H5_TypeID_Arr_NLvInt    = H5Tarray_create( H5T_NATIVE_INT,    1, &H5_ArrDims_NLv       );
   const hid_t   H5_TypeID_Arr_NLvLong   = H5Tarray_create( H5T_NATIVE_LONG,   1, &H5_ArrDims_NLv       );
   const hid_t   H5_TypeID_Arr_NLvDouble = H5Tarray_create( H5T_NATIVE_DOUBLE, 1, &H5_ArrDims_NLv       );


// create the "variable-length string" datatype
   hid_t  H5_TypeID_VarStr;
   herr_t H5_Status;

   H5_TypeID_VarStr = H5Tcopy( H5T_C_S1 );
   H5_Status        = H5Tset_size( H5_TypeID_VarStr, H5T_VARIABLE );


// get the compound type
   H5_TypeID = H5Tcreate( H5T_COMPOUND, sizeof(KeyInfo_t) );

   H5Tinsert( H5_TypeID, "FormatVersion",      HOFFSET(KeyInfo_t,FormatVersion  ),    H5T_NATIVE_INT          );
   H5Tinsert( H5_TypeID, "Model",              HOFFSET(KeyInfo_t,Model          ),    H5T_NATIVE_INT          );
   H5Tinsert( H5_TypeID, "Float8",             HOFFSET(KeyInfo_t,Float8         ),    H5T_NATIVE_INT          );
   H5Tinsert( H5_TypeID, "Gravity",            HOFFSET(KeyInfo_t,Gravity        ),    H5T_NATIVE_INT          );
   H5Tinsert( H5_TypeID, "Particle",           HOFFSET(KeyInfo_t,Particle       ),    H5T_NATIVE_INT          );
   H5Tinsert( H5_TypeID, "NLevel",             HOFFSET(KeyInfo_t,NLevel         ),    H5T_NATIVE_INT          );
   H5Tinsert( H5_TypeID, "PatchSize",          HOFFSET(KeyInfo_t,PatchSize      ),    H5T_NATIVE_INT          );
   H5Tinsert( H5_TypeID, "DumpID",             HOFFSET(KeyInfo_t,DumpID         ),    H5T_NATIVE_INT          );
#  ifdef GRAVITY
   H5Tinsert( H5_TypeID, "OutputPot",          HOFFSET(KeyInfo_t,OutputPot      ),    H5T_NATIVE_INT          );
#  endif
   H5Tinsert( H5_TypeID, "NX0",                HOFFSET(KeyInfo_t,NX0            ),    H5_TypeID_Arr_3Int      );
   H5Tinsert( H5_TypeID, "BoxScale",           HOFFSET(KeyInfo_t,BoxScale       ),    H5_TypeID_Arr_3Int      );
   H5Tinsert( H5_TypeID, "NPatch",             HOFFSET(KeyInfo_t,NPatch         ),    H5_TypeID_Arr_NLvInt    );
   H5Tinsert( H5_TypeID, "CellScale",          HOFFSET(KeyInfo_t,CellScale      ),    H5_TypeID_Arr_NLvInt    );

   H5Tinsert( H5_TypeID, "Step",               HOFFSET(KeyInfo_t,Step           ),    H5T_NATIVE_LONG         );
   H5Tinsert( H5_TypeID, "AdvanceCounter",     HOFFSET(KeyInfo_t,AdvanceCounter ),    H5_TypeID_Arr_NLvLong   );

   H5Tinsert( H5_TypeID, "BoxSize",            HOFFSET(KeyInfo_t,BoxSize        ),    H5_TypeID_Arr_3Double   );
   H5Tinsert( H5_TypeID, "Time",               HOFFSET(KeyInfo_t,Time           ),    H5_TypeID_Arr_NLvDouble );
   H5Tinsert( H5_TypeID, "CellSize",           HOFFSET(KeyInfo_t,CellSize       ),    H5_TypeID_Arr_NLvDouble );
#  ifdef GRAVITY
   H5Tinsert( H5_TypeID, "AveDens",            HOFFSET(KeyInfo_t,AveDens        ),    H5T_NATIVE_DOUBLE       );
#  endif

   H5Tinsert( H5_TypeID, "CodeVersion",        HOFFSET(KeyInfo_t,CodeVersion    ),    H5_TypeID_VarStr        );
   H5Tinsert( H5_TypeID, "DumpWallTime",       HOFFSET(KeyInfo_t,DumpWallTime   ),    H5_TypeID_VarStr        );


// free memory
   H5_Status = H5Tclose( H5_TypeID_Arr_3Double       );
   H5_Status = H5Tclose( H5_TypeID_Arr_3Int          );
   H5_Status = H5Tclose( H5_TypeID_Arr_NLvInt        );
   H5_Status = H5Tclose( H5_TypeID_Arr_NLvLong       );
   H5_Status = H5Tclose( H5_TypeID_Arr_NLvDouble     );
   H5_Status = H5Tclose( H5_TypeID_VarStr            );

} // FUNCTION : GetCompound_KeyInfo



//-------------------------------------------------------------------------------------------------------
// Function    :  GetCompound_Makefile
// Description :  Create the HDF5 compound datatype for Makefile
//
// Note        :  1. Data sturcture is defined in "HDF5_Typedef.h"
//                2. The returned H5_TypeID must be closed manually
//                3. Call-by-reference
//
// Parameter   :  H5_TypeID   : HDF5 type ID for storing the compound datatype
//-------------------------------------------------------------------------------------------------------
void GetCompound_Makefile( hid_t &H5_TypeID )
{

   H5_TypeID = H5Tcreate( H5T_COMPOUND, sizeof(Makefile_t) );

   H5Tinsert( H5_TypeID, "Model",              HOFFSET(Makefile_t,Model             ), H5T_NATIVE_INT );
   H5Tinsert( H5_TypeID, "Gravity",            HOFFSET(Makefile_t,Gravity           ), H5T_NATIVE_INT );
   H5Tinsert( H5_TypeID, "IndividualDt",       HOFFSET(Makefile_t,IndividualDt      ), H5T_NATIVE_INT );
   H5Tinsert( H5_TypeID, "Comoving",           HOFFSET(Makefile_t,Comoving          ), H5T_NATIVE_INT );
   H5Tinsert( H5_TypeID, "Particle",           HOFFSET(Makefile_t,Particle          ), H5T_NATIVE_INT );

   H5Tinsert( H5_TypeID, "UseGPU",             HOFFSET(Makefile_t,UseGPU            ), H5T_NATIVE_INT );
   H5Tinsert( H5_TypeID, "GAMER_Optimization", HOFFSET(Makefile_t,GAMER_Optimization), H5T_NATIVE_INT );
   H5Tinsert( H5_TypeID, "GAMER_Debug",        HOFFSET(Makefile_t,GAMER_Debug       ), H5T_NATIVE_INT );
   H5Tinsert( H5_TypeID, "Timing",             HOFFSET(Makefile_t,Timing            ), H5T_NATIVE_INT );
   H5Tinsert( H5_TypeID, "TimingSolver",       HOFFSET(Makefile_t,TimingSolver      ), H5T_NATIVE_INT );
   H5Tinsert( H5_TypeID, "Intel",              HOFFSET(Makefile_t,Intel             ), H5T_NATIVE_INT );
   H5Tinsert( H5_TypeID, "Float8",             HOFFSET(Makefile_t,Float8            ), H5T_NATIVE_INT );
   H5Tinsert( H5_TypeID, "Serial",             HOFFSET(Makefile_t,Serial            ), H5T_NATIVE_INT );
   H5Tinsert( H5_TypeID, "LoadBalance",        HOFFSET(Makefile_t,LoadBalance       ), H5T_NATIVE_INT );
   H5Tinsert( H5_TypeID, "OverlapMPI",         HOFFSET(Makefile_t,OverlapMPI        ), H5T_NATIVE_INT );
   H5Tinsert( H5_TypeID, "OpenMP",             HOFFSET(Makefile_t,OpenMP            ), H5T_NATIVE_INT );
   H5Tinsert( H5_TypeID, "GPU_Arch",           HOFFSET(Makefile_t,GPU_Arch          ), H5T_NATIVE_INT );
   H5Tinsert( H5_TypeID, "Laohu",              HOFFSET(Makefile_t,Laohu             ), H5T_NATIVE_INT );
   H5Tinsert( H5_TypeID, "SupportHDF5",        HOFFSET(Makefile_t,SupportHDF5       ), H5T_NATIVE_INT );

   H5Tinsert( H5_TypeID, "NLevel",             HOFFSET(Makefile_t,NLevel            ), H5T_NATIVE_INT );
   H5Tinsert( H5_TypeID, "MaxPatch",           HOFFSET(Makefile_t,MaxPatch          ), H5T_NATIVE_INT );

#  ifdef GRAVITY
   H5Tinsert( H5_TypeID, "PotScheme",          HOFFSET(Makefile_t,PotScheme         ), H5T_NATIVE_INT );
   H5Tinsert( H5_TypeID, "StorePotGhost",      HOFFSET(Makefile_t,StorePotGhost     ), H5T_NATIVE_INT );
   H5Tinsert( H5_TypeID, "UnsplitGravity",     HOFFSET(Makefile_t,UnsplitGravity    ), H5T_NATIVE_INT );
#  endif

#  if   ( MODEL == HYDRO )
   H5Tinsert( H5_TypeID, "FluScheme",          HOFFSET(Makefile_t,FluScheme         ), H5T_NATIVE_INT );
#  ifdef LR_SCHEME
   H5Tinsert( H5_TypeID, "LRScheme",           HOFFSET(Makefile_t,LRScheme          ), H5T_NATIVE_INT );
#  endif
#  ifdef RSOLVER
   H5Tinsert( H5_TypeID, "RSolver",            HOFFSET(Makefile_t,RSolver           ), H5T_NATIVE_INT );
#  endif
   H5Tinsert( H5_TypeID, "NPassive",           HOFFSET(Makefile_t,NPassive          ), H5T_NATIVE_INT );

#  elif ( MODEL == MHD )
#  warning : WAIT MHD !!!

#  elif ( MODEL == ELBDM )
   H5Tinsert( H5_TypeID, "ConserveMass",       HOFFSET(Makefile_t,ConserveMass      ), H5T_NATIVE_INT );
   H5Tinsert( H5_TypeID, "Laplacian4th",       HOFFSET(Makefile_t,Laplacian4th      ), H5T_NATIVE_INT );
   H5Tinsert( H5_TypeID, "SelfInteraction4",   HOFFSET(Makefile_t,SelfInteraction4  ), H5T_NATIVE_INT );

#  else
#  error : unsupported MODEL !!
#  endif // MODEL

} // FUNCTION : GetCompound_Makefile



//-------------------------------------------------------------------------------------------------------
// Function    :  GetCompound_SymConst
// Description :  Create the HDF5 compound datatype for SymConst
//
// Note        :  1. Data sturcture is defined in "HDF5_Typedef.h"
//                2. The returned H5_TypeID must be closed manually
//                3. Call-by-reference
//
// Parameter   :  H5_TypeID   : HDF5 type ID for storing the compound datatype
//-------------------------------------------------------------------------------------------------------
void GetCompound_SymConst( hid_t &H5_TypeID )
{

   H5_TypeID = H5Tcreate( H5T_COMPOUND, sizeof(SymConst_t) );

   H5Tinsert( H5_TypeID, "NComp",                HOFFSET(SymConst_t,NComp               ), H5T_NATIVE_INT    );
   H5Tinsert( H5_TypeID, "PatchSize",            HOFFSET(SymConst_t,PatchSize           ), H5T_NATIVE_INT    );
   H5Tinsert( H5_TypeID, "Flu_NIn",              HOFFSET(SymConst_t,Flu_NIn             ), H5T_NATIVE_INT    );
   H5Tinsert( H5_TypeID, "Flu_NOut",             HOFFSET(SymConst_t,Flu_NOut            ), H5T_NATIVE_INT    );
   H5Tinsert( H5_TypeID, "NFlux",                HOFFSET(SymConst_t,NFlux               ), H5T_NATIVE_INT    );
   H5Tinsert( H5_TypeID, "Flu_GhostSize",        HOFFSET(SymConst_t,Flu_GhostSize       ), H5T_NATIVE_INT    );
   H5Tinsert( H5_TypeID, "Flu_Nxt",              HOFFSET(SymConst_t,Flu_Nxt             ), H5T_NATIVE_INT    );
   H5Tinsert( H5_TypeID, "Debug_HDF5",           HOFFSET(SymConst_t,Debug_HDF5          ), H5T_NATIVE_INT    );
   H5Tinsert( H5_TypeID, "SibOffsetNonperiodic", HOFFSET(SymConst_t,SibOffsetNonperiodic), H5T_NATIVE_INT    );
#  ifdef LOAD_BALANCE
   H5Tinsert( H5_TypeID, "SonOffsetLB",          HOFFSET(SymConst_t,SonOffsetLB         ), H5T_NATIVE_INT    );
#  endif
   H5Tinsert( H5_TypeID, "TinyValue",            HOFFSET(SymConst_t,TinyValue           ), H5T_NATIVE_DOUBLE );

#  ifdef GRAVITY
   H5Tinsert( H5_TypeID, "Gra_NIn",              HOFFSET(SymConst_t,Gra_NIn             ), H5T_NATIVE_INT    );
   H5Tinsert( H5_TypeID, "Pot_GhostSize",        HOFFSET(SymConst_t,Pot_GhostSize       ), H5T_NATIVE_INT    );
   H5Tinsert( H5_TypeID, "Gra_GhostSize",        HOFFSET(SymConst_t,Gra_GhostSize       ), H5T_NATIVE_INT    );
   H5Tinsert( H5_TypeID, "Rho_GhostSize",        HOFFSET(SymConst_t,Rho_GhostSize       ), H5T_NATIVE_INT    );
   H5Tinsert( H5_TypeID, "Pot_Nxt",              HOFFSET(SymConst_t,Pot_Nxt             ), H5T_NATIVE_INT    );
   H5Tinsert( H5_TypeID, "Gra_Nxt",              HOFFSET(SymConst_t,Gra_Nxt             ), H5T_NATIVE_INT    );
   H5Tinsert( H5_TypeID, "Rho_Nxt",              HOFFSET(SymConst_t,Rho_Nxt             ), H5T_NATIVE_INT    );
#  ifdef UNSPLIT_GRAVITY
   H5Tinsert( H5_TypeID, "USG_GhostSize",        HOFFSET(SymConst_t,USG_GhostSize       ), H5T_NATIVE_INT    );
   H5Tinsert( H5_TypeID, "USG_NxtF",             HOFFSET(SymConst_t,USG_NxtF            ), H5T_NATIVE_INT    );
   H5Tinsert( H5_TypeID, "USG_NxtG",             HOFFSET(SymConst_t,USG_NxtG            ), H5T_NATIVE_INT    );
#  endif
   H5Tinsert( H5_TypeID, "Gra_BlockSize_z",      HOFFSET(SymConst_t,Gra_BlockSize_z     ), H5T_NATIVE_INT    );
   H5Tinsert( H5_TypeID, "ExtPotNAuxMax",        HOFFSET(SymConst_t,ExtPotNAuxMax       ), H5T_NATIVE_INT    );
   H5Tinsert( H5_TypeID, "ExtAccNAuxMax",        HOFFSET(SymConst_t,ExtAccNAuxMax       ), H5T_NATIVE_INT    );
#  if   ( POT_SCHEME == SOR )
   H5Tinsert( H5_TypeID, "Pot_BlockSize_z",      HOFFSET(SymConst_t,Pot_BlockSize_z     ), H5T_NATIVE_INT    );
   H5Tinsert( H5_TypeID, "UsePSolver_10to14",    HOFFSET(SymConst_t,UsePSolver_10to14   ), H5T_NATIVE_INT    );
#  elif ( POT_SCHEME == MG  )
   H5Tinsert( H5_TypeID, "Pot_BlockSize_x",      HOFFSET(SymConst_t,Pot_BlockSize_x     ), H5T_NATIVE_INT    );
#  endif
#  endif // #ifdef GRAVITY

#  ifdef PARTICLE
   H5Tinsert( H5_TypeID, "NPar_Var",             HOFFSET(SymConst_t,NPar_Var            ), H5T_NATIVE_INT    );
   H5Tinsert( H5_TypeID, "NPar_Passive",         HOFFSET(SymConst_t,NPar_Passive        ), H5T_NATIVE_INT    );
   H5Tinsert( H5_TypeID, "Debug_Particle",       HOFFSET(SymConst_t,Debug_Particle      ), H5T_NATIVE_INT    );
   H5Tinsert( H5_TypeID, "ParList_GrowthFactor", HOFFSET(SymConst_t,ParList_GrowthFactor), H5T_NATIVE_DOUBLE );
   H5Tinsert( H5_TypeID, "ParList_ReduceFactor", HOFFSET(SymConst_t,ParList_ReduceFactor), H5T_NATIVE_DOUBLE );
#  endif

#  if   ( MODEL == HYDRO )
   H5Tinsert( H5_TypeID, "Flu_BlockSize_x",      HOFFSET(SymConst_t,Flu_BlockSize_x     ), H5T_NATIVE_INT    );
   H5Tinsert( H5_TypeID, "Flu_BlockSize_y",      HOFFSET(SymConst_t,Flu_BlockSize_y     ), H5T_NATIVE_INT    );
   H5Tinsert( H5_TypeID, "CheckNegativeInFluid", HOFFSET(SymConst_t,CheckNegativeInFluid), H5T_NATIVE_INT    );
   H5Tinsert( H5_TypeID, "CharReconstruction",   HOFFSET(SymConst_t,CharReconstruction  ), H5T_NATIVE_INT    );
   H5Tinsert( H5_TypeID, "CheckIntermediate",    HOFFSET(SymConst_t,CheckIntermediate   ), H5T_NATIVE_INT    );
   H5Tinsert( H5_TypeID, "HLL_NoRefState",       HOFFSET(SymConst_t,HLL_NoRefState      ), H5T_NATIVE_INT    );
   H5Tinsert( H5_TypeID, "HLL_IncludeAllWaves",  HOFFSET(SymConst_t,HLL_IncludeAllWaves ), H5T_NATIVE_INT    );
   H5Tinsert( H5_TypeID, "WAF_Dissipate",        HOFFSET(SymConst_t,WAF_Dissipate       ), H5T_NATIVE_INT    );
   H5Tinsert( H5_TypeID, "PositiveDensInFixUp",  HOFFSET(SymConst_t,PositiveDensInFixUp ), H5T_NATIVE_INT    );
#  ifdef N_FC_VAR
   H5Tinsert( H5_TypeID, "N_FC_Var",             HOFFSET(SymConst_t,N_FC_Var            ), H5T_NATIVE_INT    );
#  endif
#  ifdef N_SLOPE_PPM
   H5Tinsert( H5_TypeID, "N_Slope_PPM",          HOFFSET(SymConst_t,N_Slope_PPM         ), H5T_NATIVE_INT    );
#  endif
#  ifdef MIN_PRES_DENS
   H5Tinsert( H5_TypeID, "Min_Pres_Dens",        HOFFSET(SymConst_t,Min_Pres_Dens       ), H5T_NATIVE_DOUBLE );
#  endif
#  ifdef MIN_PRES
   H5Tinsert( H5_TypeID, "Min_Pres",             HOFFSET(SymConst_t,Min_Pres            ), H5T_NATIVE_DOUBLE );
#  endif
#  ifdef MAX_ERROR
   H5Tinsert( H5_TypeID, "MaxError",             HOFFSET(SymConst_t,MaxError            ), H5T_NATIVE_DOUBLE );
#  endif

#  elif ( MODEL == MHD )
   H5Tinsert( H5_TypeID, "Flu_BlockSize_x",      HOFFSET(SymConst_t,Flu_BlockSize_x     ), H5T_NATIVE_INT    );
   H5Tinsert( H5_TypeID, "Flu_BlockSize_y",      HOFFSET(SymConst_t,Flu_BlockSize_y     ), H5T_NATIVE_INT    );
#  warning : WAIT MHD !!!

#  elif  ( MODEL == ELBDM )
   H5Tinsert( H5_TypeID, "Flu_BlockSize_x",      HOFFSET(SymConst_t,Flu_BlockSize_x     ), H5T_NATIVE_INT    );
   H5Tinsert( H5_TypeID, "Flu_BlockSize_y",      HOFFSET(SymConst_t,Flu_BlockSize_y     ), H5T_NATIVE_INT    );

#  else
#  error : ERROR : unsupported MODEL !!
#  endif // MODEL

} // FUNCTION : GetCompound_SymConst



//-------------------------------------------------------------------------------------------------------
// Function    :  GetCompound_InputPara
// Description :  Create the HDF5 compound datatype for InputPara
//
// Note        :  1. Data sturcture is defined in "HDF5_Typedef.h"
//                2. The returned H5_TypeID must be closed manually
//                3. Call-by-reference
//
// Parameter   :  H5_TypeID   : HDF5 type ID for storing the compound datatype
//-------------------------------------------------------------------------------------------------------
void GetCompound_InputPara( hid_t &H5_TypeID )
{

// create the array type
   const hsize_t H5_ArrDims_3Var             = 3;                    // array size of [3]
   const hsize_t H5_ArrDims_6Var             = 6;                    // array size of [6]
   const hsize_t H5_ArrDims_NLvM1            = NLEVEL-1;             // array size of [NLEVEL-1]
   const hsize_t H5_ArrDims_NLvM1_2[2]       = { NLEVEL-1, 2 };      // array size of [NLEVEL-1][2]
   const hsize_t H5_ArrDims_NLvM1_3[2]       = { NLEVEL-1, 3 };      // array size of [NLEVEL-1][3]

   const hid_t   H5_TypeID_Arr_3Int          = H5Tarray_create( H5T_NATIVE_INT,    1, &H5_ArrDims_3Var      );
   const hid_t   H5_TypeID_Arr_6Int          = H5Tarray_create( H5T_NATIVE_INT,    1, &H5_ArrDims_6Var      );
   const hid_t   H5_TypeID_Arr_NLvM1Double   = H5Tarray_create( H5T_NATIVE_DOUBLE, 1, &H5_ArrDims_NLvM1     );
   const hid_t   H5_TypeID_Arr_NLvM1_2Double = H5Tarray_create( H5T_NATIVE_DOUBLE, 2,  H5_ArrDims_NLvM1_2   );
   const hid_t   H5_TypeID_Arr_NLvM1_3Double = H5Tarray_create( H5T_NATIVE_DOUBLE, 2,  H5_ArrDims_NLvM1_3   );

   herr_t  H5_Status;


// get the compound type
   H5_TypeID = H5Tcreate( H5T_COMPOUND, sizeof(InputPara_t) );

// simulation scale
   H5Tinsert( H5_TypeID, "BoxSize",                 HOFFSET(InputPara_t,BoxSize                ), H5T_NATIVE_DOUBLE  );
   H5Tinsert( H5_TypeID, "NX0_Tot",                 HOFFSET(InputPara_t,NX0_Tot                ), H5_TypeID_Arr_3Int );
   H5Tinsert( H5_TypeID, "MPI_NRank",               HOFFSET(InputPara_t,MPI_NRank              ), H5T_NATIVE_INT     );
   H5Tinsert( H5_TypeID, "MPI_NRank_X",             HOFFSET(InputPara_t,MPI_NRank_X            ), H5_TypeID_Arr_3Int );
   H5Tinsert( H5_TypeID, "OMP_NThread",             HOFFSET(InputPara_t,OMP_NThread            ), H5T_NATIVE_INT     );
   H5Tinsert( H5_TypeID, "EndT",                    HOFFSET(InputPara_t,EndT                   ), H5T_NATIVE_DOUBLE  );
   H5Tinsert( H5_TypeID, "EndStep",                 HOFFSET(InputPara_t,EndStep                ), H5T_NATIVE_LONG    );

// boundary condition
   H5Tinsert( H5_TypeID, "Opt__BC_Flu",             HOFFSET(InputPara_t,Opt__BC_Flu            ), H5_TypeID_Arr_6Int );
#  ifdef GRAVITY
   H5Tinsert( H5_TypeID, "Opt__BC_Pot",             HOFFSET(InputPara_t,Opt__BC_Pot            ), H5T_NATIVE_INT     );
   H5Tinsert( H5_TypeID, "GFunc_Coeff0",            HOFFSET(InputPara_t,GFunc_Coeff0           ), H5T_NATIVE_DOUBLE  );
#  endif

// particle
#  ifdef PARTICLE
   H5Tinsert( H5_TypeID, "Par_NPar",                HOFFSET(InputPara_t,Par_NPar               ), H5T_NATIVE_LONG    );
   H5Tinsert( H5_TypeID, "Par_Init",                HOFFSET(InputPara_t,Par_Init               ), H5T_NATIVE_INT     );
   H5Tinsert( H5_TypeID, "Par_Interp",              HOFFSET(InputPara_t,Par_Interp             ), H5T_NATIVE_INT     );
   H5Tinsert( H5_TypeID, "Par_Integ",               HOFFSET(InputPara_t,Par_Integ              ), H5T_NATIVE_INT     );
   H5Tinsert( H5_TypeID, "Par_ImproveAcc",          HOFFSET(InputPara_t,Par_ImproveAcc         ), H5T_NATIVE_INT     );
   H5Tinsert( H5_TypeID, "Par_PredictPos",          HOFFSET(InputPara_t,Par_PredictPos         ), H5T_NATIVE_INT     );
   H5Tinsert( H5_TypeID, "Par_RemoveCell",          HOFFSET(InputPara_t,Par_RemoveCell         ), H5T_NATIVE_INT     );
#  endif

// cosmology
#  ifdef COMOVING
   H5Tinsert( H5_TypeID, "A_Init",                  HOFFSET(InputPara_t,A_Init                 ), H5T_NATIVE_DOUBLE  );
   H5Tinsert( H5_TypeID, "OmegaM0",                 HOFFSET(InputPara_t,OmegaM0                ), H5T_NATIVE_DOUBLE  );
#  endif

// time-step determination
   H5Tinsert( H5_TypeID, "Dt__Fluid",               HOFFSET(InputPara_t,Dt__Fluid              ), H5T_NATIVE_DOUBLE  );
   H5Tinsert( H5_TypeID, "Dt__FluidInit",           HOFFSET(InputPara_t,Dt__FluidInit          ), H5T_NATIVE_DOUBLE  );
#  ifdef GRAVITY
   H5Tinsert( H5_TypeID, "Dt__Gravity",             HOFFSET(InputPara_t,Dt__Gravity            ), H5T_NATIVE_DOUBLE  );
#  endif
#  if ( MODEL == ELBDM )
   H5Tinsert( H5_TypeID, "Dt__Phase",               HOFFSET(InputPara_t,Dt__Phase              ), H5T_NATIVE_DOUBLE  );
#  endif
#  ifdef PARTICLE 
   H5Tinsert( H5_TypeID, "Dt__ParVel",              HOFFSET(InputPara_t,Dt__ParVel             ), H5T_NATIVE_DOUBLE  );
   H5Tinsert( H5_TypeID, "Dt__ParVelMax",           HOFFSET(InputPara_t,Dt__ParVelMax          ), H5T_NATIVE_DOUBLE  );
#  endif
#  ifdef COMOVING
   H5Tinsert( H5_TypeID, "Dt__MaxDeltaA",           HOFFSET(InputPara_t,Dt__MaxDeltaA          ), H5T_NATIVE_DOUBLE  );
#  endif
   H5Tinsert( H5_TypeID, "Opt__AdaptiveDt",         HOFFSET(InputPara_t,Opt__AdaptiveDt        ), H5T_NATIVE_INT     );
   H5Tinsert( H5_TypeID, "Opt__RecordDt",           HOFFSET(InputPara_t,Opt__RecordDt          ), H5T_NATIVE_INT     );
   H5Tinsert( H5_TypeID, "Opt__DtUser",             HOFFSET(InputPara_t,Opt__DtUser            ), H5T_NATIVE_INT     );
   

// domain refinement
   H5Tinsert( H5_TypeID, "RegridCount",             HOFFSET(InputPara_t,RegridCount            ), H5T_NATIVE_INT     );
   H5Tinsert( H5_TypeID, "FlagBufferSize",          HOFFSET(InputPara_t,FlagBufferSize         ), H5T_NATIVE_INT     );
   H5Tinsert( H5_TypeID, "MaxLevel",                HOFFSET(InputPara_t,MaxLevel               ), H5T_NATIVE_INT     );
   H5Tinsert( H5_TypeID, "Opt__Flag_Rho",           HOFFSET(InputPara_t,Opt__Flag_Rho          ), H5T_NATIVE_INT     );
   H5Tinsert( H5_TypeID, "Opt__Flag_RhoGradient",   HOFFSET(InputPara_t,Opt__Flag_RhoGradient  ), H5T_NATIVE_INT     );
#  if ( MODEL == HYDRO ) 
   H5Tinsert( H5_TypeID, "Opt__Flag_PresGradient",  HOFFSET(InputPara_t,Opt__Flag_PresGradient ), H5T_NATIVE_INT     );
#  endif
#  if ( MODEL == ELBDM ) 
   H5Tinsert( H5_TypeID, "Opt__Flag_EngyDensity",   HOFFSET(InputPara_t,Opt__Flag_EngyDensity  ), H5T_NATIVE_INT     );
#  endif
   H5Tinsert( H5_TypeID, "Opt__Flag_LohnerDens",    HOFFSET(InputPara_t,Opt__Flag_LohnerDens   ), H5T_NATIVE_INT     );
#  if ( MODEL == HYDRO ) 
   H5Tinsert( H5_TypeID, "Opt__Flag_LohnerEngy",    HOFFSET(InputPara_t,Opt__Flag_LohnerEngy   ), H5T_NATIVE_INT     );
   H5Tinsert( H5_TypeID, "Opt__Flag_LohnerPres",    HOFFSET(InputPara_t,Opt__Flag_LohnerPres   ), H5T_NATIVE_INT     );
#  endif
   H5Tinsert( H5_TypeID, "Opt__Flag_LohnerForm",    HOFFSET(InputPara_t,Opt__Flag_LohnerForm   ), H5T_NATIVE_INT     );
   H5Tinsert( H5_TypeID, "Opt__Flag_User",          HOFFSET(InputPara_t,Opt__Flag_User         ), H5T_NATIVE_INT     );
   H5Tinsert( H5_TypeID, "Opt__Flag_Region",        HOFFSET(InputPara_t,Opt__Flag_Region       ), H5T_NATIVE_INT     );
   H5Tinsert( H5_TypeID, "Opt__PatchCount",         HOFFSET(InputPara_t,Opt__PatchCount        ), H5T_NATIVE_INT     );
#  ifdef PARTICLE
   H5Tinsert( H5_TypeID, "Opt__ParLevel",           HOFFSET(InputPara_t,Opt__ParLevel          ), H5T_NATIVE_INT     );
#  endif

// load balance
#  ifdef LOAD_BALANCE
   H5Tinsert( H5_TypeID, "LB_Input__WLI_Max",       HOFFSET(InputPara_t,LB_Input__WLI_Max      ), H5T_NATIVE_DOUBLE  );
#  endif

// fluid solvers in HYDRO
#  if ( MODEL == HYDRO )
   H5Tinsert( H5_TypeID, "Gamma",                   HOFFSET(InputPara_t,Gamma                  ), H5T_NATIVE_DOUBLE  );
   H5Tinsert( H5_TypeID, "MinMod_Coeff",            HOFFSET(InputPara_t,MinMod_Coeff           ), H5T_NATIVE_DOUBLE  );
   H5Tinsert( H5_TypeID, "EP_Coeff",                HOFFSET(InputPara_t,EP_Coeff               ), H5T_NATIVE_DOUBLE  );
   H5Tinsert( H5_TypeID, "Opt__LR_Limiter",         HOFFSET(InputPara_t,Opt__LR_Limiter        ), H5T_NATIVE_INT     );
   H5Tinsert( H5_TypeID, "Opt__WAF_Limiter",        HOFFSET(InputPara_t,Opt__WAF_Limiter       ), H5T_NATIVE_INT     );
   H5Tinsert( H5_TypeID, "Opt__CorrUnphyScheme",    HOFFSET(InputPara_t,Opt__CorrUnphyScheme   ), H5T_NATIVE_INT     );
#  endif

// ELBDM solvers
#  if ( MODEL == ELBDM )
   H5Tinsert( H5_TypeID, "ELBDM_Mass",              HOFFSET(InputPara_t,ELBDM_Mass             ), H5T_NATIVE_DOUBLE  );
   H5Tinsert( H5_TypeID, "ELBDM_PlanckConst",       HOFFSET(InputPara_t,ELBDM_PlanckConst      ), H5T_NATIVE_DOUBLE  );
#  ifdef QUARTIC_SELF_INTERACTION
   H5Tinsert( H5_TypeID, "ELBDM_Lambda",            HOFFSET(InputPara_t,ELBDM_Lambda           ), H5T_NATIVE_DOUBLE  );
#  endif
   H5Tinsert( H5_TypeID, "ELBDM_Taylor3_Coeff",     HOFFSET(InputPara_t,ELBDM_Taylor3_Coeff    ), H5T_NATIVE_DOUBLE  );
   H5Tinsert( H5_TypeID, "ELBDM_Taylor3_Auto",      HOFFSET(InputPara_t,ELBDM_Taylor3_Auto     ), H5T_NATIVE_INT     );
#  endif

// fluid solvers in both HYDRO/MHD/ELBDM
   H5Tinsert( H5_TypeID, "Flu_GPU_NPGroup",         HOFFSET(InputPara_t,Flu_GPU_NPGroup        ), H5T_NATIVE_INT     );
   H5Tinsert( H5_TypeID, "GPU_NStream",             HOFFSET(InputPara_t,GPU_NStream            ), H5T_NATIVE_INT     );
   H5Tinsert( H5_TypeID, "Opt__FixUp_Flux",         HOFFSET(InputPara_t,Opt__FixUp_Flux        ), H5T_NATIVE_INT     );
   H5Tinsert( H5_TypeID, "Opt__FixUp_Restrict",     HOFFSET(InputPara_t,Opt__FixUp_Restrict    ), H5T_NATIVE_INT     );
   H5Tinsert( H5_TypeID, "Opt__OverlapMPI",         HOFFSET(InputPara_t,Opt__OverlapMPI        ), H5T_NATIVE_INT     );
   H5Tinsert( H5_TypeID, "Opt__ResetFluid",         HOFFSET(InputPara_t,Opt__ResetFluid        ), H5T_NATIVE_INT     );
   H5Tinsert( H5_TypeID, "Opt__CorrUnphy",          HOFFSET(InputPara_t,Opt__CorrUnphy         ), H5T_NATIVE_INT     );

// self-gravity
#  ifdef GRAVITY
   H5Tinsert( H5_TypeID, "NewtonG",                 HOFFSET(InputPara_t,NewtonG                ), H5T_NATIVE_DOUBLE  );
#  if   ( POT_SCHEME == SOR )
   H5Tinsert( H5_TypeID, "SOR_Omega",               HOFFSET(InputPara_t,SOR_Omega              ), H5T_NATIVE_DOUBLE  );
   H5Tinsert( H5_TypeID, "SOR_MaxIter",             HOFFSET(InputPara_t,SOR_MaxIter            ), H5T_NATIVE_INT     );
   H5Tinsert( H5_TypeID, "SOR_MinIter",             HOFFSET(InputPara_t,SOR_MinIter            ), H5T_NATIVE_INT     );
#  elif ( POT_SCHEME == MG )
   H5Tinsert( H5_TypeID, "MG_MaxIter",              HOFFSET(InputPara_t,MG_MaxIter             ), H5T_NATIVE_INT     );
   H5Tinsert( H5_TypeID, "MG_NPreSmooth",           HOFFSET(InputPara_t,MG_NPreSmooth          ), H5T_NATIVE_INT     );
   H5Tinsert( H5_TypeID, "MG_NPostSmooth",          HOFFSET(InputPara_t,MG_NPostSmooth         ), H5T_NATIVE_INT     );
   H5Tinsert( H5_TypeID, "MG_ToleratedError",       HOFFSET(InputPara_t,MG_ToleratedError      ), H5T_NATIVE_DOUBLE  );
#  endif
   H5Tinsert( H5_TypeID, "Pot_GPU_NPGroup",         HOFFSET(InputPara_t,Pot_GPU_NPGroup        ), H5T_NATIVE_INT     );
   H5Tinsert( H5_TypeID, "Opt__GraP5Gradient",      HOFFSET(InputPara_t,Opt__GraP5Gradient     ), H5T_NATIVE_INT     );
   H5Tinsert( H5_TypeID, "Opt__GravityType",        HOFFSET(InputPara_t,Opt__GravityType       ), H5T_NATIVE_INT     );
   H5Tinsert( H5_TypeID, "Opt__ExternalPot",        HOFFSET(InputPara_t,Opt__ExternalPot       ), H5T_NATIVE_INT     );
#  endif

// initialization
   H5Tinsert( H5_TypeID, "Opt__Init",               HOFFSET(InputPara_t,Opt__Init              ), H5T_NATIVE_INT     );
   H5Tinsert( H5_TypeID, "Opt__RestartHeader",      HOFFSET(InputPara_t,Opt__RestartHeader     ), H5T_NATIVE_INT     );
   H5Tinsert( H5_TypeID, "Opt__UM_Start_Level",     HOFFSET(InputPara_t,Opt__UM_Start_Level    ), H5T_NATIVE_INT     );
   H5Tinsert( H5_TypeID, "Opt__UM_Start_NVar",      HOFFSET(InputPara_t,Opt__UM_Start_NVar     ), H5T_NATIVE_INT     );
   H5Tinsert( H5_TypeID, "Opt__UM_Start_Downgrade", HOFFSET(InputPara_t,Opt__UM_Start_Downgrade), H5T_NATIVE_INT     );
   H5Tinsert( H5_TypeID, "Opt__UM_Start_Refine",    HOFFSET(InputPara_t,Opt__UM_Start_Refine   ), H5T_NATIVE_INT     );
   H5Tinsert( H5_TypeID, "Opt__UM_Factor_5over3",   HOFFSET(InputPara_t,Opt__UM_Factor_5over3  ), H5T_NATIVE_INT     );
   H5Tinsert( H5_TypeID, "Opt__InitRestrict",       HOFFSET(InputPara_t,Opt__InitRestrict      ), H5T_NATIVE_INT     );
   H5Tinsert( H5_TypeID, "Opt__GPUID_Select",       HOFFSET(InputPara_t,Opt__GPUID_Select      ), H5T_NATIVE_INT     );
   H5Tinsert( H5_TypeID, "Init_Subsampling_NCell",  HOFFSET(InputPara_t,Init_Subsampling_NCell ), H5T_NATIVE_INT     );

// interpolation schemes
   H5Tinsert( H5_TypeID, "Opt__Int_Time",           HOFFSET(InputPara_t,Opt__Int_Time          ), H5T_NATIVE_INT     );
#  if ( MODEL == ELBDM ) 
   H5Tinsert( H5_TypeID, "Opt__Int_Phase",          HOFFSET(InputPara_t,Opt__Int_Phase         ), H5T_NATIVE_INT     );
#  endif
   H5Tinsert( H5_TypeID, "Opt__Flu_IntScheme",      HOFFSET(InputPara_t,Opt__Flu_IntScheme     ), H5T_NATIVE_INT     );
#  ifdef GRAVITY
   H5Tinsert( H5_TypeID, "Opt__Pot_IntScheme",      HOFFSET(InputPara_t,Opt__Pot_IntScheme     ), H5T_NATIVE_INT     );
   H5Tinsert( H5_TypeID, "Opt__Rho_IntScheme",      HOFFSET(InputPara_t,Opt__Rho_IntScheme     ), H5T_NATIVE_INT     );
   H5Tinsert( H5_TypeID, "Opt__Gra_IntScheme",      HOFFSET(InputPara_t,Opt__Gra_IntScheme     ), H5T_NATIVE_INT     );
#  endif
   H5Tinsert( H5_TypeID, "Opt__RefFlu_IntScheme",   HOFFSET(InputPara_t,Opt__RefFlu_IntScheme  ), H5T_NATIVE_INT     );
#  ifdef GRAVITY
   H5Tinsert( H5_TypeID, "Opt__RefPot_IntScheme",   HOFFSET(InputPara_t,Opt__RefPot_IntScheme  ), H5T_NATIVE_INT     );
#  endif
   H5Tinsert( H5_TypeID, "IntMonoCoeff",            HOFFSET(InputPara_t,IntMonoCoeff           ), H5T_NATIVE_DOUBLE  );

// data dump
   H5Tinsert( H5_TypeID, "Opt__Output_Total",       HOFFSET(InputPara_t,Opt__Output_Total      ), H5T_NATIVE_INT     );
   H5Tinsert( H5_TypeID, "Opt__Output_Part",        HOFFSET(InputPara_t,Opt__Output_Part       ), H5T_NATIVE_INT     );
   H5Tinsert( H5_TypeID, "Opt__Output_TestError",   HOFFSET(InputPara_t,Opt__Output_TestError  ), H5T_NATIVE_INT     );
#  ifdef PARTICLE
   H5Tinsert( H5_TypeID, "Opt__Output_Particle",    HOFFSET(InputPara_t,Opt__Output_Particle   ), H5T_NATIVE_INT     );
#  endif
   H5Tinsert( H5_TypeID, "Opt__Output_BasePS",      HOFFSET(InputPara_t,Opt__Output_BasePS     ), H5T_NATIVE_INT     );
   H5Tinsert( H5_TypeID, "Opt__Output_Base",        HOFFSET(InputPara_t,Opt__Output_Base       ), H5T_NATIVE_INT     );
#  ifdef GRAVITY
   H5Tinsert( H5_TypeID, "Opt__Output_Pot",         HOFFSET(InputPara_t,Opt__Output_Pot        ), H5T_NATIVE_INT     );
#  endif
   H5Tinsert( H5_TypeID, "Opt__Output_Mode",        HOFFSET(InputPara_t,Opt__Output_Mode       ), H5T_NATIVE_INT     );
   H5Tinsert( H5_TypeID, "Opt__Output_Step",        HOFFSET(InputPara_t,Opt__Output_Step       ), H5T_NATIVE_INT     );
   H5Tinsert( H5_TypeID, "Opt__Output_Dt",          HOFFSET(InputPara_t,Opt__Output_Dt         ), H5T_NATIVE_DOUBLE  );
   H5Tinsert( H5_TypeID, "Output_PartX",            HOFFSET(InputPara_t,Output_PartX           ), H5T_NATIVE_DOUBLE  );
   H5Tinsert( H5_TypeID, "Output_PartY",            HOFFSET(InputPara_t,Output_PartY           ), H5T_NATIVE_DOUBLE  );
   H5Tinsert( H5_TypeID, "Output_PartZ",            HOFFSET(InputPara_t,Output_PartZ           ), H5T_NATIVE_DOUBLE  );
   H5Tinsert( H5_TypeID, "InitDumpID",              HOFFSET(InputPara_t,InitDumpID             ), H5T_NATIVE_INT     );

// miscellaneous
   H5Tinsert( H5_TypeID, "Opt__Verbose",            HOFFSET(InputPara_t,Opt__Verbose           ), H5T_NATIVE_INT     );
   H5Tinsert( H5_TypeID, "Opt__TimingBalance",      HOFFSET(InputPara_t,Opt__TimingBalance     ), H5T_NATIVE_INT     );
   H5Tinsert( H5_TypeID, "Opt__TimingMPI",          HOFFSET(InputPara_t,Opt__TimingMPI         ), H5T_NATIVE_INT     );
   H5Tinsert( H5_TypeID, "Opt__RecordMemory",       HOFFSET(InputPara_t,Opt__RecordMemory      ), H5T_NATIVE_INT     );
   H5Tinsert( H5_TypeID, "Opt__RecordPerformance",  HOFFSET(InputPara_t,Opt__RecordPerformance ), H5T_NATIVE_INT     );
   H5Tinsert( H5_TypeID, "Opt__ManualControl",      HOFFSET(InputPara_t,Opt__ManualControl     ), H5T_NATIVE_INT     );
   H5Tinsert( H5_TypeID, "Opt__RecordUser",         HOFFSET(InputPara_t,Opt__RecordUser        ), H5T_NATIVE_INT     );

// simulation checks
   H5Tinsert( H5_TypeID, "Opt__Ck_Refine",          HOFFSET(InputPara_t,Opt__Ck_Refine         ), H5T_NATIVE_INT     );
   H5Tinsert( H5_TypeID, "Opt__Ck_ProperNesting",   HOFFSET(InputPara_t,Opt__Ck_ProperNesting  ), H5T_NATIVE_INT     );
   H5Tinsert( H5_TypeID, "Opt__Ck_Conservation",    HOFFSET(InputPara_t,Opt__Ck_Conservation   ), H5T_NATIVE_INT     );
   H5Tinsert( H5_TypeID, "Opt__Ck_Restrict",        HOFFSET(InputPara_t,Opt__Ck_Restrict       ), H5T_NATIVE_INT     );
   H5Tinsert( H5_TypeID, "Opt__Ck_Finite",          HOFFSET(InputPara_t,Opt__Ck_Finite         ), H5T_NATIVE_INT     );
   H5Tinsert( H5_TypeID, "Opt__Ck_PatchAllocate",   HOFFSET(InputPara_t,Opt__Ck_PatchAllocate  ), H5T_NATIVE_INT     );
   H5Tinsert( H5_TypeID, "Opt__Ck_FluxAllocate",    HOFFSET(InputPara_t,Opt__Ck_FluxAllocate   ), H5T_NATIVE_INT     );
#  if ( MODEL == HYDRO )
   H5Tinsert( H5_TypeID, "Opt__Ck_Negative",        HOFFSET(InputPara_t,Opt__Ck_Negative       ), H5T_NATIVE_INT     );
#  endif
   H5Tinsert( H5_TypeID, "Opt__Ck_MemFree",         HOFFSET(InputPara_t,Opt__Ck_MemFree        ), H5T_NATIVE_DOUBLE  );
#  ifdef PARTICLE
   H5Tinsert( H5_TypeID, "Opt__Ck_Particle",        HOFFSET(InputPara_t,Opt__Ck_Particle       ), H5T_NATIVE_INT     );
#  endif

// flag tables
   H5Tinsert( H5_TypeID, "FlagTable_Rho",          HOFFSET(InputPara_t,FlagTable_Rho           ), H5_TypeID_Arr_NLvM1Double   );
   H5Tinsert( H5_TypeID, "FlagTable_RhoGradient",  HOFFSET(InputPara_t,FlagTable_RhoGradient   ), H5_TypeID_Arr_NLvM1Double   );
   H5Tinsert( H5_TypeID, "FlagTable_Lohner",       HOFFSET(InputPara_t,FlagTable_Lohner        ), H5_TypeID_Arr_NLvM1_3Double );
   H5Tinsert( H5_TypeID, "FlagTable_User",         HOFFSET(InputPara_t,FlagTable_User          ), H5_TypeID_Arr_NLvM1Double   );
#  if   ( MODEL == HYDRO )
   H5Tinsert( H5_TypeID, "FlagTable_PresGradient", HOFFSET(InputPara_t,FlagTable_PresGradient  ), H5_TypeID_Arr_NLvM1Double   );
#  elif ( MODEL == ELBDM )
   H5Tinsert( H5_TypeID, "FlagTable_EngyDensity",  HOFFSET(InputPara_t,FlagTable_EngyDensity   ), H5_TypeID_Arr_NLvM1_2Double );
#  endif


// free memory
   H5_Status = H5Tclose( H5_TypeID_Arr_3Int          );
   H5_Status = H5Tclose( H5_TypeID_Arr_6Int          );
   H5_Status = H5Tclose( H5_TypeID_Arr_NLvM1Double   );
   H5_Status = H5Tclose( H5_TypeID_Arr_NLvM1_2Double );
   H5_Status = H5Tclose( H5_TypeID_Arr_NLvM1_3Double );

} // FUNCTION : GetCompound_InputPara



#endif // #ifdef SUPPORT_HDF5