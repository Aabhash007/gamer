#include "Copyright.h"
#include "GAMER.h"
#include "CUFLU.h"

#if ( defined MIN_PRES_DENS  ||  defined MIN_PRES )
extern real CPU_PositivePres_In_Engy( const real ConVar[], const real Gamma_m1, const real _Gamma_m1 );
#endif




//-------------------------------------------------------------------------------------------------------
// Function    :  Flu_FixUp
// Description :  1. Use the corrected coarse-fine boundary fluxes to fix the data at level "lv"
//                2. Use the average data at level "lv+1" to replace the data at level "lv"
//
// Note        :  1. Also include the fluxes from neighbor ranks
//                2. The boundary fluxes must be received in advance by invoking the function "Buf_GetBufferData"
//
// Parameter   :  lv : Targeted refinement level
//                dt : Time interval to advance solution
//-------------------------------------------------------------------------------------------------------
void Flu_FixUp( const int lv, const double dt )
{

   const real Const = dt / amr->dh[lv];
   const int  FluSg = amr->FluSg[lv];

   real CorrVal; // corrected value
   real (*FluxPtr)[PATCH_SIZE][PATCH_SIZE] = NULL;

#  if ( MODEL == ELBDM  &&  defined CONSERVE_MASS )
   real Re, Im, Rho_Wrong, Rho_Corr, Rescale;
#  endif

#  if ( defined MIN_PRES_DENS  ||  defined MIN_PRES )
   const real  Gamma_m1 = GAMMA - (real)1.0;
   const real _Gamma_m1 = (real)1.0 / Gamma_m1;

   real Fluid[NCOMP];
#  endif


// a. correct the coarse-fine boundary fluxes
   if ( OPT__FIXUP_FLUX )
   {
//    check
#     ifdef GAMER_DEBUG

      if ( !amr->WithFlux )    
         Aux_Error( ERROR_INFO, "amr->WithFlux is off -> no flux array is allocated for OPT__FIXUP_FLUX !!\n" );

#     if ( MODEL == ELBDM )
#     ifndef CONSERVE_MASS
      Aux_Error( ERROR_INFO, "CONSERVE_MASS is not turned on in the Makefile for the option OPT__FIXUP_FLUX !!\n" );
#     endif

#     if ( NFLUX != 1 )
      Aux_Error( ERROR_INFO, "NFLUX (%d) != 1 for the option OPT__FIXUP_FLUX !!\n", NFLUX );
#     endif

#     if ( DENS != 0 )
      Aux_Error( ERROR_INFO, "DENS (%d) != 0 for the option OPT__FIXUP_FLUX !!\n", DENS );
#     endif

#     if ( FLUX_DENS != 0 )
      Aux_Error( ERROR_INFO, "FLUX_DENS (%d) != 0 for the option OPT__FIXUP_FLUX !!\n", FLUX_DENS );
#     endif
#     endif // #if ( MODEL == ELBDM )

#     endif // #ifdef GAMER_DEBUG


#     if ( MODEL == ELBDM  &&  defined CONSERVE_MASS )
#     pragma omp parallel for private( CorrVal, FluxPtr, Re, Im, Rho_Wrong, Rho_Corr, Rescale ) schedule( runtime )
#     elif ( defined MIN_PRES_DENS  ||  defined MIN_PRES )
#     pragma omp parallel for private( CorrVal, FluxPtr, Fluid ) schedule( runtime )
#     endif
      for (int PID=0; PID<amr->NPatchComma[lv][1]; PID++)
      {
//       a1. sum up the coarse-grid and fine-grid fluxes for the debug mode
#        ifdef GAMER_DEBUG
         for (int s=0; s<6; s++)
         {
            FluxPtr = amr->patch[0][lv][PID]->flux[s];

            if ( FluxPtr != NULL )
            {
               for (int v=0; v<NFLUX; v++)
               for (int m=0; m<PS1; m++)
               for (int n=0; n<PS1; n++)
                  FluxPtr[v][m][n] += amr->patch[0][lv][PID]->flux_debug[s][v][m][n];
            }
         }
#        endif


//       a2. correct fluid variables by the difference between coarse-grid and fine-grid fluxes
         if ( NULL != (FluxPtr = amr->patch[0][lv][PID]->flux[0]) )
         {
            for (int v=0; v<NFLUX; v++)
            for (int k=0; k<PATCH_SIZE; k++)
            for (int j=0; j<PATCH_SIZE; j++)
            {
               CorrVal = amr->patch[FluSg][lv][PID]->fluid[v][k][j][           0] - FluxPtr[v][k][j] * Const;
#              ifdef POSITIVE_DENS_IN_FIXUP
               if ( v != DENS  ||  CorrVal > (real)0.0 )
#              endif
               amr->patch[FluSg][lv][PID]->fluid[v][k][j][           0] = CorrVal;

//             rescale the real and imaginary parts to be consistent with the corrected amplitude
#              if ( MODEL == ELBDM  &&  defined CONSERVE_MASS )
               Re        = amr->patch[FluSg][lv][PID]->fluid[REAL][k][j][0];
               Im        = amr->patch[FluSg][lv][PID]->fluid[IMAG][k][j][0];
               Rho_Corr  = amr->patch[FluSg][lv][PID]->fluid[DENS][k][j][0];
               Rho_Wrong = SQR(Re) + SQR(Im);

//             be careful about the negative density introduced from the round-off errors
               if ( Rho_Wrong <= (real)0.0  ||  Rho_Corr <= (real)0.0 )
               {
                  amr->patch[FluSg][lv][PID]->fluid[DENS][k][j][0] = (real)0.0;
                  Rescale = (real)0.0;
               }
               else
                  Rescale = SQRT( Rho_Corr/Rho_Wrong );

               amr->patch[FluSg][lv][PID]->fluid[REAL][k][j][0] *= Rescale;
               amr->patch[FluSg][lv][PID]->fluid[IMAG][k][j][0] *= Rescale;
#              endif
            }

//          ensure the positive pressure
#           if ( defined MIN_PRES_DENS  ||  defined MIN_PRES )
            for (int k=0; k<PATCH_SIZE; k++)
            for (int j=0; j<PATCH_SIZE; j++)
            {
               for (int v=0; v<NCOMP; v++)   Fluid[v] = amr->patch[FluSg][lv][PID]->fluid[v][k][j][           0];

               amr->patch[FluSg][lv][PID]->fluid[ENGY][k][j][           0] = CPU_PositivePres_In_Engy( Fluid, Gamma_m1, _Gamma_m1 );
            }
#           endif
         } // if ( flux[0] != NULL )

         if ( NULL != (FluxPtr = amr->patch[0][lv][PID]->flux[1]) )
         {
            for (int v=0; v<NFLUX; v++)
            for (int k=0; k<PATCH_SIZE; k++)
            for (int j=0; j<PATCH_SIZE; j++)
            {
               CorrVal = amr->patch[FluSg][lv][PID]->fluid[v][k][j][PATCH_SIZE-1] + FluxPtr[v][k][j] * Const;
#              ifdef POSITIVE_DENS_IN_FIXUP
               if ( v != DENS  ||  CorrVal > (real)0.0 )
#              endif
               amr->patch[FluSg][lv][PID]->fluid[v][k][j][PATCH_SIZE-1] = CorrVal;

//             rescale the real and imaginary parts to be consistent with the corrected amplitude
#              if ( MODEL == ELBDM  &&  defined CONSERVE_MASS )
               Re        = amr->patch[FluSg][lv][PID]->fluid[REAL][k][j][PATCH_SIZE-1];
               Im        = amr->patch[FluSg][lv][PID]->fluid[IMAG][k][j][PATCH_SIZE-1];
               Rho_Corr  = amr->patch[FluSg][lv][PID]->fluid[DENS][k][j][PATCH_SIZE-1];
               Rho_Wrong = SQR(Re) + SQR(Im);

//             be careful about the negative density introduced from the round-off errors
               if ( Rho_Wrong <= (real)0.0  ||  Rho_Corr <= (real)0.0 )
               {
                  amr->patch[FluSg][lv][PID]->fluid[DENS][k][j][PATCH_SIZE-1] = (real)0.0;
                  Rescale = (real)0.0;
               }
               else
                  Rescale = SQRT( Rho_Corr/Rho_Wrong );

               amr->patch[FluSg][lv][PID]->fluid[REAL][k][j][PATCH_SIZE-1] *= Rescale;
               amr->patch[FluSg][lv][PID]->fluid[IMAG][k][j][PATCH_SIZE-1] *= Rescale;
#              endif
            }

//          ensure the positive pressure
#           if ( defined MIN_PRES_DENS  ||  defined MIN_PRES )
            for (int k=0; k<PATCH_SIZE; k++)
            for (int j=0; j<PATCH_SIZE; j++)
            {
               for (int v=0; v<NCOMP; v++)   Fluid[v] = amr->patch[FluSg][lv][PID]->fluid[v][k][j][PATCH_SIZE-1];

               amr->patch[FluSg][lv][PID]->fluid[ENGY][k][j][PATCH_SIZE-1] = CPU_PositivePres_In_Engy( Fluid, Gamma_m1, _Gamma_m1 );
            }
#           endif
         } // if ( flux[1] != NULL )

         if ( NULL != (FluxPtr = amr->patch[0][lv][PID]->flux[2]) )
         {
            for (int v=0; v<NFLUX; v++)
            for (int k=0; k<PATCH_SIZE; k++)
            for (int i=0; i<PATCH_SIZE; i++)
            {
               CorrVal = amr->patch[FluSg][lv][PID]->fluid[v][k][           0][i] - FluxPtr[v][k][i] * Const;
#              ifdef POSITIVE_DENS_IN_FIXUP
               if ( v != DENS  ||  CorrVal > (real)0.0 )
#              endif
               amr->patch[FluSg][lv][PID]->fluid[v][k][           0][i] = CorrVal;

//             rescale the real and imaginary parts to be consistent with the corrected amplitude
#              if ( MODEL == ELBDM  &&  defined CONSERVE_MASS )
               Re        = amr->patch[FluSg][lv][PID]->fluid[REAL][k][0][i];
               Im        = amr->patch[FluSg][lv][PID]->fluid[IMAG][k][0][i];
               Rho_Corr  = amr->patch[FluSg][lv][PID]->fluid[DENS][k][0][i];
               Rho_Wrong = SQR(Re) + SQR(Im);

//             be careful about the negative density introduced from the round-off errors
               if ( Rho_Wrong <= (real)0.0  ||  Rho_Corr <= (real)0.0 )
               {
                  amr->patch[FluSg][lv][PID]->fluid[DENS][k][0][i] = (real)0.0;
                  Rescale = (real)0.0;
               }
               else
                  Rescale = SQRT( Rho_Corr/Rho_Wrong );

               amr->patch[FluSg][lv][PID]->fluid[REAL][k][0][i] *= Rescale;
               amr->patch[FluSg][lv][PID]->fluid[IMAG][k][0][i] *= Rescale;
#              endif
            }

//          ensure the positive pressure
#           if ( defined MIN_PRES_DENS  ||  defined MIN_PRES )
            for (int k=0; k<PATCH_SIZE; k++)
            for (int i=0; i<PATCH_SIZE; i++)
            {
               for (int v=0; v<NCOMP; v++)   Fluid[v] = amr->patch[FluSg][lv][PID]->fluid[v][k][           0][i];

               amr->patch[FluSg][lv][PID]->fluid[ENGY][k][           0][i] = CPU_PositivePres_In_Engy( Fluid, Gamma_m1, _Gamma_m1 );
            }
#           endif
         } // if ( flux[2] != NULL )

         if ( NULL != (FluxPtr = amr->patch[0][lv][PID]->flux[3]) )
         {
            for (int v=0; v<NFLUX; v++)
            for (int k=0; k<PATCH_SIZE; k++)
            for (int i=0; i<PATCH_SIZE; i++)
            {
               CorrVal = amr->patch[FluSg][lv][PID]->fluid[v][k][PATCH_SIZE-1][i] + FluxPtr[v][k][i] * Const;
#              ifdef POSITIVE_DENS_IN_FIXUP
               if ( v != DENS  ||  CorrVal > (real)0.0 )
#              endif
               amr->patch[FluSg][lv][PID]->fluid[v][k][PATCH_SIZE-1][i] = CorrVal;

//             rescale the real and imaginary parts to be consistent with the corrected amplitude
#              if ( MODEL == ELBDM  &&  defined CONSERVE_MASS )
               Re        = amr->patch[FluSg][lv][PID]->fluid[REAL][k][PATCH_SIZE-1][i];
               Im        = amr->patch[FluSg][lv][PID]->fluid[IMAG][k][PATCH_SIZE-1][i];
               Rho_Corr  = amr->patch[FluSg][lv][PID]->fluid[DENS][k][PATCH_SIZE-1][i];
               Rho_Wrong = SQR(Re) + SQR(Im);

//             be careful about the negative density introduced from the round-off errors
               if ( Rho_Wrong <= (real)0.0  ||  Rho_Corr <= (real)0.0 )
               {
                  amr->patch[FluSg][lv][PID]->fluid[DENS][k][PATCH_SIZE-1][i] = (real)0.0;
                  Rescale = (real)0.0;
               }
               else
                  Rescale = SQRT( Rho_Corr/Rho_Wrong );

               amr->patch[FluSg][lv][PID]->fluid[REAL][k][PATCH_SIZE-1][i] *= Rescale;
               amr->patch[FluSg][lv][PID]->fluid[IMAG][k][PATCH_SIZE-1][i] *= Rescale;
#              endif
            }

//          ensure the positive pressure
#           if ( defined MIN_PRES_DENS  ||  defined MIN_PRES )
            for (int k=0; k<PATCH_SIZE; k++)
            for (int i=0; i<PATCH_SIZE; i++)
            {
               for (int v=0; v<NCOMP; v++)   Fluid[v] = amr->patch[FluSg][lv][PID]->fluid[v][k][PATCH_SIZE-1][i];

               amr->patch[FluSg][lv][PID]->fluid[ENGY][k][PATCH_SIZE-1][i] = CPU_PositivePres_In_Engy( Fluid, Gamma_m1, _Gamma_m1 );
            }
#           endif
         } // if ( flux[3] != NULL )

         if ( NULL != (FluxPtr = amr->patch[0][lv][PID]->flux[4]) )
         {
            for (int v=0; v<NFLUX; v++)
            for (int j=0; j<PATCH_SIZE; j++)
            for (int i=0; i<PATCH_SIZE; i++)
            {
               CorrVal = amr->patch[FluSg][lv][PID]->fluid[v][           0][j][i] - FluxPtr[v][j][i] * Const;
#              ifdef POSITIVE_DENS_IN_FIXUP
               if ( v != DENS  ||  CorrVal > (real)0.0 )
#              endif
               amr->patch[FluSg][lv][PID]->fluid[v][           0][j][i] = CorrVal;

//             rescale the real and imaginary parts to be consistent with the corrected amplitude
#              if ( MODEL == ELBDM  &&  defined CONSERVE_MASS )
               Re        = amr->patch[FluSg][lv][PID]->fluid[REAL][0][j][i];
               Im        = amr->patch[FluSg][lv][PID]->fluid[IMAG][0][j][i];
               Rho_Corr  = amr->patch[FluSg][lv][PID]->fluid[DENS][0][j][i];
               Rho_Wrong = SQR(Re) + SQR(Im);

//             be careful about the negative density introduced from the round-off errors
               if ( Rho_Wrong <= (real)0.0  ||  Rho_Corr <= (real)0.0 )
               {
                  amr->patch[FluSg][lv][PID]->fluid[DENS][0][j][i] = (real)0.0;
                  Rescale = (real)0.0;
               }
               else
                  Rescale = SQRT( Rho_Corr/Rho_Wrong );

               amr->patch[FluSg][lv][PID]->fluid[REAL][0][j][i] *= Rescale;
               amr->patch[FluSg][lv][PID]->fluid[IMAG][0][j][i] *= Rescale;
#              endif
            }

//          ensure the positive pressure
#           if ( defined MIN_PRES_DENS  ||  defined MIN_PRES )
            for (int j=0; j<PATCH_SIZE; j++)
            for (int i=0; i<PATCH_SIZE; i++)
            {
               for (int v=0; v<NCOMP; v++)   Fluid[v] = amr->patch[FluSg][lv][PID]->fluid[v][           0][j][i];

               amr->patch[FluSg][lv][PID]->fluid[ENGY][           0][j][i] = CPU_PositivePres_In_Engy( Fluid, Gamma_m1, _Gamma_m1 );
            }
#           endif
         } // if ( flux[4] != NULL )

         if ( NULL != (FluxPtr = amr->patch[0][lv][PID]->flux[5]) )
         {
            for (int v=0; v<NFLUX; v++)
            for (int j=0; j<PATCH_SIZE; j++)
            for (int i=0; i<PATCH_SIZE; i++)
            {
               CorrVal = amr->patch[FluSg][lv][PID]->fluid[v][PATCH_SIZE-1][j][i] + FluxPtr[v][j][i] * Const;
#              ifdef POSITIVE_DENS_IN_FIXUP
               if ( v != DENS  ||  CorrVal > (real)0.0 )
#              endif
               amr->patch[FluSg][lv][PID]->fluid[v][PATCH_SIZE-1][j][i] = CorrVal;

//             rescale the real and imaginary parts to be consistent with the corrected amplitude
#              if ( MODEL == ELBDM  &&  defined CONSERVE_MASS )
               Re        = amr->patch[FluSg][lv][PID]->fluid[REAL][PATCH_SIZE-1][j][i];
               Im        = amr->patch[FluSg][lv][PID]->fluid[IMAG][PATCH_SIZE-1][j][i];
               Rho_Corr  = amr->patch[FluSg][lv][PID]->fluid[DENS][PATCH_SIZE-1][j][i];
               Rho_Wrong = SQR(Re) + SQR(Im);

//             be careful about the negative density introduced from the round-off errors
               if ( Rho_Wrong <= (real)0.0  ||  Rho_Corr <= (real)0.0 )
               {
                  amr->patch[FluSg][lv][PID]->fluid[DENS][PATCH_SIZE-1][j][i] = (real)0.0;
                  Rescale = (real)0.0;
               }
               else
                  Rescale = SQRT( Rho_Corr/Rho_Wrong );

               amr->patch[FluSg][lv][PID]->fluid[REAL][PATCH_SIZE-1][j][i] *= Rescale;
               amr->patch[FluSg][lv][PID]->fluid[IMAG][PATCH_SIZE-1][j][i] *= Rescale;
#              endif
            }

//          ensure the positive pressure
#           if ( defined MIN_PRES_DENS  ||  defined MIN_PRES )
            for (int j=0; j<PATCH_SIZE; j++)
            for (int i=0; i<PATCH_SIZE; i++)
            {
               for (int v=0; v<NCOMP; v++)   Fluid[v] = amr->patch[FluSg][lv][PID]->fluid[v][PATCH_SIZE-1][j][i];

               amr->patch[FluSg][lv][PID]->fluid[ENGY][PATCH_SIZE-1][j][i] = CPU_PositivePres_In_Engy( Fluid, Gamma_m1, _Gamma_m1 );
            }
#           endif
         } // if ( flux[5] != NULL )

      } // for (int PID=0; PID<amr->NPatchComma[lv][1]; PID++)


//    a3. reset all flux arrays (in both real and buffer patches) to zero for the debug mode
#     ifdef GAMER_DEBUG
#     pragma omp parallel for private( FluxPtr ) schedule( runtime )
      for (int PID=0; PID<amr->NPatchComma[lv][27]; PID++)
      {
         for (int s=0; s<6; s++)
         {
            FluxPtr = amr->patch[0][lv][PID]->flux[s];
            if ( FluxPtr != NULL )
            {
               for (int v=0; v<NFLUX; v++)
               for (int m=0; m<PS1; m++)
               for (int n=0; n<PS1; n++)
                  FluxPtr[v][m][n] = 0.0;
            }

            FluxPtr = amr->patch[0][lv][PID]->flux_debug[s];
            if ( FluxPtr != NULL )
            {
               for (int v=0; v<NFLUX; v++)
               for (int m=0; m<PS1; m++)
               for (int n=0; n<PS1; n++)
                  FluxPtr[v][m][n] = 0.0;
            }
         }
      }
#     endif

   } // if ( OPT__FIXUP_FLUX )


// b. average over the data at level "lv+1" to correct the data at level "lv"
   if ( OPT__FIXUP_RESTRICT )    Flu_Restrict( lv, amr->FluSg[lv+1], amr->FluSg[lv], NULL_INT, NULL_INT, _FLU );

} // FUNCTION : Flu_FixUp