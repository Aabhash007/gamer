#include "Copyright.h"
#include "GAMER.h"

#ifndef SERIAL




//-------------------------------------------------------------------------------------------------------
// Function    :  Buf_GetBufferData
// Description :  Fill up the data of the buffer patches / Exchange the buffer fluxes for the fix-up operation
//
// Note        :  The modes "POT_FOR_POISSON" and "POT_AFTER_REFINE" can be applied to the potential 
//                data only. For others modes, the number of variables to be exchanged depends on 
//                the input parameter "TVar".
//
// Parameter   :  lv          : Targeted refinement level to exchage data  
//                FluSg       : Sandglass of the requested fluid data (useless in POT_FOR_POISSON, 
//                              POT_AFTER_REFINEPOT, COARSE_FINE_FLUX )
//                PotSg       : Sandglass of the requested potential data (useless in COARSE_FINE_FLUX)
//                GetBufMode  : Targeted mode. Each mode has its own MPI lists, by which the amount of data 
//                              to be transferred can be minimized.
//                              --> DATA_GENERAL      : data for general-purpose (sibling and coarse-grid data)
//                                  DATA_AFTER_REFINE : subset of DATA_GENERAL after refine
//                                  DATA_AFTER_FIXUP  : subset of DATA_GENERAL after fix-up
//                                  DATA_RESTRICT     : restricted data of the father patches with sons not home
//                                                      --> useful in LOAD_BALANCE only
//                                  POT_FOR_POISSON   : potential for the Poisson solver
//                                  POT_AFTER_REFINE  : potential after refine for the Poisson solver
//                                  COARSE_FINE_FLUX  : fluxes across the coarse-fine boundaries (HYDRO ONLY)
//                TVar        : Targeted variables to exchange
//                              --> Supported variables in different models:
//                                  HYDRO : _DENS, _MOMX, _MOMY, _MOMZ, _ENGY, _FLU [, _POTE] [, _PASSIVE]
//                                  MHD   : 
//                                  ELBDM : _DENS, _REAL, _IMAG, _FLU [, _POTE]
//                                  In addition, the flux variables (e.g., _FLUX_DENS) are also supported
//                              Restrictions :
//                              --> a. DATA_XXX works with all components in (_FLU | _POTE | _PASSIVE)
//                                  b. COARSE_FINE_FLUX works with all components in (_FLUX | _FLUX_PASSIVE)
//                                  c. _POTE has no effect on the flux fix-up in DATA_AFTER_FIXUP
//                                  d. POT_FOR_POISSON and POT_AFTER_REFINE only work with _POTE
//                ParaBuf     : Number of ghost zones to exchange (useless in DATA_RESTRICT and COARSE_FINE_FLUX )
//                UseLBFunc   : Use the load-balance alternative function "LB_GetBufferData" 
//                              (useless if LOAD_BALANCE is off)
//                              --> USELB_YES : use the load-balance alternative function
//                                  USELB_NO  : do not use the load-balance alternative function
//-------------------------------------------------------------------------------------------------------
void Buf_GetBufferData( const int lv, const int FluSg, const int PotSg, const GetBufMode_t GetBufMode, 
                        const int TVar, const int ParaBuf, const UseLBFunc_t UseLBFunc )
{

// invoke the alternative load-balance function   
#  ifdef LOAD_BALANCE 
   if ( UseLBFunc == USELB_YES )
   {
      LB_GetBufferData( lv, FluSg, PotSg, GetBufMode, TVar, ParaBuf );
      return;
   }
#  endif


// check
   if ( lv < 0  ||  lv >= NLEVEL )
      Aux_Error( ERROR_INFO, "incorrect parameter %s = %d !!\n", "lv", lv );

   if ( GetBufMode == DATA_RESTRICT )
      Aux_Error( ERROR_INFO, "mode DATA_RESTRICT is useful only in LOAD_BALANCE !!\n" );

   if (  ( TVar & (_FLU|_PASSIVE) )  &&  ( FluSg != 0 && FluSg != 1 )  &&  GetBufMode != COARSE_FINE_FLUX )
      Aux_Error( ERROR_INFO, "incorrect parameter %s = %d !!\n", "FluSg", FluSg );

#  ifdef GRAVITY
   if (  ( TVar & _POTE )  &&  ( PotSg != 0 && PotSg != 1 )  &&  GetBufMode != COARSE_FINE_FLUX )
      Aux_Error( ERROR_INFO, "incorrect parameter %s = %d !!\n", "PotSg", PotSg );

   if (  ( GetBufMode == DATA_GENERAL || GetBufMode == DATA_AFTER_FIXUP || GetBufMode == DATA_AFTER_REFINE )  &&
        !( TVar & (_FLU|_POTE|_PASSIVE) )  )
      Aux_Error( ERROR_INFO, "no suitable targeted variable is found --> missing (_FLU|_POTE|_PASSIVE) !!\n" );

   if (  ( GetBufMode == POT_FOR_POISSON || GetBufMode == POT_AFTER_REFINE )  &&  !( TVar & _POTE )  )
      Aux_Error( ERROR_INFO, "no suitable targeted variable is found --> missing _POTE !!\n" );

   if (  ( GetBufMode == POT_FOR_POISSON || GetBufMode == POT_AFTER_REFINE )  &&  ( TVar & ~_POTE )  )
      Aux_Error( ERROR_INFO, "modes \"%s\" only accept \"%s\" as the targeted variable !!\n",
                 "POT_FOR_POISSON and POT_AFTER_REFINE", "_POTE" );

   if (  ( GetBufMode == DATA_GENERAL || GetBufMode == DATA_AFTER_FIXUP || GetBufMode == DATA_AFTER_REFINE ||
           GetBufMode == POT_FOR_POISSON || GetBufMode == POT_AFTER_REFINE )  && 
         ( ParaBuf < 0 || ParaBuf > PATCH_SIZE )  )
      Aux_Error( ERROR_INFO, "incorrect parameter %s = %d --> accepted range = [0 ... PATCH_SIZE] !!\n", 
                 "ParaBuf", ParaBuf );
   
#  else // #ifdef GRAVITY ... else ...
   if (  ( GetBufMode == DATA_GENERAL || GetBufMode == DATA_AFTER_FIXUP || GetBufMode == DATA_AFTER_REFINE )  &&
        !( TVar & (_FLU|_PASSIVE) )  )
      Aux_Error( ERROR_INFO, "no suitable targeted variable is found --> missing (_FLU|_PASSIVE) !!\n" );

   if (  ( GetBufMode == DATA_GENERAL || GetBufMode == DATA_AFTER_FIXUP || GetBufMode == DATA_AFTER_REFINE )  &&
         ( ParaBuf < 0 || ParaBuf > PATCH_SIZE )  )
      Aux_Error( ERROR_INFO, "incorrect parameter %s = %d --> range=[0 ... PATCH_SIZE] !!\n", 
                 "ParaBuf", ParaBuf );
#  endif // #ifdef GRAVITY ... else ...

   if (  GetBufMode == COARSE_FINE_FLUX  &&  !( TVar & (_FLUX|_FLUX_PASSIVE) )  )
      Aux_Error( ERROR_INFO, "no suitable targeted variable is found --> missing (_FLUX|_FLUX_PASSIVE) !!\n" );

   if ( GetBufMode == COARSE_FINE_FLUX  &&  !amr->WithFlux )
   {
      Aux_Message( stderr, "WARNING : mode COARSE_FINE_FLUX is useless since no flux is required !!\n" );
      return;
   }


// determine the components to be prepared (TFluVarIdx : targeted fluid variable indices ( = [0 ... NCOMP/NFLUX+NPASSIVE-1] )
   bool ExchangeFlu    = TVar & ( _FLU | _PASSIVE );  // whether or not to exchage the fluid and passive data 
#  ifdef GRAVITY
   bool ExchangePot    = TVar & _POTE;                // whether or not to exchange the potential data
#  endif

   const int NMax = ( GetBufMode == COARSE_FINE_FLUX ) ? NFLUX+NPASSIVE : NCOMP+NPASSIVE;
   int NVar_Flu, NVar_Tot, TFluVarIdx, TFluVarIdxList[NMax];
   NVar_Flu = 0;
   
   for (int v=0; v<NMax; v++)
      if ( TVar & (1<<v) )    TFluVarIdxList[ NVar_Flu++ ] = v;

   NVar_Tot = NVar_Flu;
#  ifdef GRAVITY
   if ( ExchangePot    )   NVar_Tot ++; 

   if ( GetBufMode == POT_FOR_POISSON  ||  GetBufMode == POT_AFTER_REFINE )
   {
      ExchangeFlu    = false;
      ExchangePot    = true;
      NVar_Flu       = 0;
      NVar_Tot       = 1; 
   }
#  endif

// check again
   if ( NVar_Tot == 0  ||  ( GetBufMode == COARSE_FINE_FLUX && NVar_Flu == 0 )  )
   {
      Aux_Message( stderr, "WARNING : no targeted variable is found !!\n" );
      return;
   }


// TSibList : targeted sibling direction, MaxSib : number of sibling directions to exchange data
   const int TSibList[26] = { 0,1,2,3,4,5,6,9,7,8,10,13,11,12,14,17,16,15,18,25,19,24,20,23,21,22 };
   const int MaxSib       = ( GetBufMode == COARSE_FINE_FLUX ) ? 6 : 26;

   int LoopWidth[3], Disp[2][3], Sib, MirSib, TRank[2];
   int SendSize[2], RecvSize[2], PID, Counter;
   real *SendBuffer[2] = { NULL, NULL }; 
   real *RecvBuffer[2] = { NULL, NULL };
   real (*FluxPtr)[PATCH_SIZE][PATCH_SIZE] = NULL;


// loop over all targeted sibling directions (two opposite directions at a time)
   for (int s=0; s<MaxSib; s+=2)
   {

//    1. allocate SendBuffer and RecvBuffer
//    ==================================================================================================
      for (int d=0; d<3; d++)    LoopWidth[d] = TABLE_01( TSibList[s], 'x'+d, ParaBuf, PATCH_SIZE, ParaBuf );

      for (int t=0; t<2; t++)
      {
         Sib      = TSibList[s+t];
         TRank[t] = MPI_SibRank[Sib];

         for (int d=0; d<3; d++)    Disp[t][d] = TABLE_01( Sib, 'x'+d, 0, 0, PATCH_SIZE-ParaBuf );
         
         SendSize[t] = ( GetBufMode == COARSE_FINE_FLUX ) ? 
                       amr->ParaVar->SendF_NList[lv][Sib]*PS1*PS1*NVar_Flu :
                       amr->ParaVar->SendP_NList[lv][Sib]*LoopWidth[0]*LoopWidth[1]*LoopWidth[2]*NVar_Tot;

         RecvSize[t] = ( GetBufMode == COARSE_FINE_FLUX ) ?
                       amr->ParaVar->RecvF_NList[lv][Sib]*PS1*PS1*NVar_Flu :
                       amr->ParaVar->RecvP_NList[lv][Sib]*LoopWidth[0]*LoopWidth[1]*LoopWidth[2]*NVar_Tot;

         SendBuffer[t] = new real [ SendSize[t] ];
         RecvBuffer[t] = new real [ RecvSize[t] ];
      } // for (int t=0; t<2; t++)


//    2. copy data into SendBuffer
//    ==================================================================================================
      for (int t=0; t<2; t++)
      {
         Sib     = TSibList[s+t  ];
         MirSib  = TSibList[s-t+1];
         Counter = 0;

         switch ( GetBufMode )
         {
            case DATA_GENERAL: case DATA_AFTER_REFINE: case DATA_AFTER_FIXUP:
#           ifdef GRAVITY
            case POT_FOR_POISSON: case POT_AFTER_REFINE:
#           endif
//          --------------------------------------------------------------------------
               for (int TID=0; TID<amr->ParaVar->SendP_NList[lv][Sib]; TID++)  
               {  
                  PID = amr->ParaVar->SendP_IDList[lv][Sib][TID];

//                fluid data
                  if ( ExchangeFlu )
                  for (int v=0; v<NVar_Flu; v++)
                  {  
                     TFluVarIdx = TFluVarIdxList[v]; 

                     for (int k=Disp[t][2]; k<Disp[t][2]+LoopWidth[2]; k++)
                     for (int j=Disp[t][1]; j<Disp[t][1]+LoopWidth[1]; j++)
                     for (int i=Disp[t][0]; i<Disp[t][0]+LoopWidth[0]; i++)
                        SendBuffer[t][ Counter ++ ] = amr->patch[FluSg][lv][PID]->fluid[TFluVarIdx][k][j][i];
                  }

#                 ifdef GRAVITY
//                potential data
                  if ( ExchangePot )
                  {
                     for (int k=Disp[t][2]; k<Disp[t][2]+LoopWidth[2]; k++)
                     for (int j=Disp[t][1]; j<Disp[t][1]+LoopWidth[1]; j++)
                     for (int i=Disp[t][0]; i<Disp[t][0]+LoopWidth[0]; i++)
                        SendBuffer[t][ Counter ++ ] = amr->patch[PotSg][lv][PID]->pot[k][j][i];
                  }
#                 endif
               } // for (int TID=0; TID<amr->ParaVar->SendP_NList[lv][Sib]; TID++)
               break; // case DATA_GENERAL: case DATA_AFTER_REFINE: case DATA_AFTER_FIXUP: case POT_FOR_POISSON: 
                      // case POT_AFTER_REFINE:


            case COARSE_FINE_FLUX :
//          --------------------------------------------------------------------------
               for (int TID=0; TID<amr->ParaVar->SendF_NList[lv][Sib]; TID++)   
               {
                  PID     = amr->ParaVar->SendF_IDList[lv][Sib][TID];
                  FluxPtr = amr->patch[0][lv][PID]->flux[MirSib];

                  for (int v=0; v<NVar_Flu; v++)
                  {  
                     TFluVarIdx = TFluVarIdxList[v]; 

                     for (int m=0; m<PATCH_SIZE; m++)          
                     for (int n=0; n<PATCH_SIZE; n++)          
                        SendBuffer[t][ Counter ++ ] = FluxPtr[TFluVarIdx][m][n];
                  }
               } 
               break; // case COARSE_FINE_FLUX :


            default: 
               Aux_Error( ERROR_INFO, "incorrect parameter %s = %d !!\n", "GetBufMode", GetBufMode );

         } // switch ( GetBufMode )
      } // for (int t=0; t<2; t++)
         

//    3. transfer data between different ranks
//    ==================================================================================================
      MPI_ExchangeData( TRank, SendSize, RecvSize, SendBuffer, RecvBuffer );


//    4. copy data from RecvBuffer back to the amr->patch pointer
//    ==================================================================================================
      for (int t=0; t<2; t++)
      {
         Sib     = TSibList[s+t];
         Counter = 0;

         switch ( GetBufMode )
         {
            case DATA_GENERAL: case DATA_AFTER_REFINE: case DATA_AFTER_FIXUP:
#           ifdef GRAVITY
            case POT_FOR_POISSON: case POT_AFTER_REFINE:
#           endif
//          --------------------------------------------------------------------------
               for (int TID=0; TID<amr->ParaVar->RecvP_NList[lv][Sib]; TID++)
               {  
                  PID = amr->ParaVar->RecvP_IDList[lv][Sib][TID];

//                fluid data
                  if ( ExchangeFlu )
                  for (int v=0; v<NVar_Flu; v++)
                  {  
                     TFluVarIdx = TFluVarIdxList[v]; 

                     for (int k=Disp[1-t][2]; k<Disp[1-t][2]+LoopWidth[2]; k++)
                     for (int j=Disp[1-t][1]; j<Disp[1-t][1]+LoopWidth[1]; j++)
                     for (int i=Disp[1-t][0]; i<Disp[1-t][0]+LoopWidth[0]; i++)
                        amr->patch[FluSg][lv][PID]->fluid[TFluVarIdx][k][j][i] = RecvBuffer[t][ Counter ++ ];
                  }

#                 ifdef GRAVITY
//                potential data
                  if ( ExchangePot )
                  {  
                     for (int k=Disp[1-t][2]; k<Disp[1-t][2]+LoopWidth[2]; k++)
                     for (int j=Disp[1-t][1]; j<Disp[1-t][1]+LoopWidth[1]; j++)
                     for (int i=Disp[1-t][0]; i<Disp[1-t][0]+LoopWidth[0]; i++)
                        amr->patch[PotSg][lv][PID]->pot[k][j][i] = RecvBuffer[t][ Counter ++ ];
                  }
#                 endif
               } // for (int TID=0; TID<amr->ParaVar->RecvP_NList[lv][Sib]; TID++)
               break; // case DATA_GENERAL: case DATA_AFTER_REFINE: case DATA_AFTER_FIXUP: case POT_FOR_POISSON: 
                      // case POT_AFTER_REFINE:


            case COARSE_FINE_FLUX :
//          --------------------------------------------------------------------------
               for (int TID=0; TID<amr->ParaVar->RecvF_NList[lv][Sib]; TID++)   
               {
                  PID     = amr->ParaVar->RecvF_IDList[lv][Sib][TID];
                  FluxPtr = amr->patch[0][lv][PID]->flux[Sib];

                  for (int v=0; v<NVar_Flu; v++)
                  {  
                     TFluVarIdx = TFluVarIdxList[v]; 

                     for (int m=0; m<PATCH_SIZE; m++)          
                     for (int n=0; n<PATCH_SIZE; n++)          
                        FluxPtr[TFluVarIdx][m][n] += RecvBuffer[t][ Counter ++ ];
                  }
               } // for (int TID=0; TID<amr->ParaVar->RecvF_NList[lv][Sib]; TID++)
               break; // case COARSE_FINE_FLUX :


            default: 
               Aux_Error( ERROR_INFO, "incorrect parameter %s = %d !!\n", "GetBufMode", GetBufMode );

         } // switch ( GetBufMode )
      } // for (int t=0; t<2; t++)

      for (int t=0; t<2; t++)
      {
         delete [] SendBuffer[t];
         delete [] RecvBuffer[t];
      }

   } // for (int s=0; s<MaxSib; s+=2)

} // FUNCTION : Buf_GetBufferData



#endif // #ifndef SERIAL