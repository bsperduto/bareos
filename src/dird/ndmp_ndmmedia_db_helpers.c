/*
   BAREOS® - Backup Archiving REcovery Open Sourced

   Copyright (C) 2011-2015 Planets Communications B.V.
   Copyright (C) 2013-2015 Bareos GmbH & Co. KG

   This program is Free Software; you can redistribute it and/or
   modify it under the terms of version three of the GNU Affero General Public
   License as published by the Free Software Foundation and included
   in the file LICENSE.

   This program is distributed in the hope that it will be useful, but
   WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
   Affero General Public License for more details.

   You should have received a copy of the GNU Affero General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
   02110-1301, USA.
*/
/*
 * Routines that store the NDMP Media Info in the Bareos DB
 * and retrieve them back
 *
 * Philipp Storz, November 2016
 */

#include "bareos.h"
#include "dird.h"

#if HAVE_NDMP

#include "ndmp/ndmagents.h"


/**
 * Store ndmmedia information into database in MEDIA and JOBMEDIA information.
 * we map the ndm_media fields into the MEDIA_DBR and JOBMEDIA_DBR fields as follows
 *
 ***********************************************************************************************
 *     NDM_MEDIA                        |        MEDIA_DBR / JOBMEDIA_DBR                      *
 ***********************************************************************************************
 char     label[NDMMEDIA_LABEL_MAX+1];  |  MEDIA_DBR:      char VolumeName[MAX_NAME_LENGTH];
 unsigned slot_addr;                    |  MEDIA_DBR:      int32_t Slot;
 -----------------------------------------------------------------------------------------------
 unsigned file_mark_offset;             |  JOBMEDIA_DBR:   uint32_t StartBlock;
 uint64_t n_bytes;                      |  JOBMEDIA_DBR:   uint64_t JobBytes;
 ***********************************************************************************************

 Notes:
   * Slot needs to be translated from bareos slotnr to NDMP slot number  and back
   * uint64_t begin_offset,end_offset; do not need to be stored (scratchpad)
 */


void ndmmedia_to_bareos_db_records(ndmmedia *media, MEDIA_DBR *mr, JOBMEDIA_DBR *jm)
{
   bstrncpy(mr->VolumeName, media->label, NDMMEDIA_LABEL_MAX);
   mr->Slot = media->slot_addr;

   jm->StartBlock = media->file_mark_offset;
   jm->JobBytes = media->n_bytes;

   /*
    * We usually get here with the Volstatus used as it is set
    * to be sure the medium will not be used by other jobs
    *
    * If eom was reached, mark medium Full,
    * else make it append so that the remaining space
    * will be used
    */
   if (media->media_eom) {
      bstrncpy(mr->VolStatus, NT_("Full"), sizeof(mr->VolStatus));
   } else {
      bstrncpy(mr->VolStatus, NT_("Append"), sizeof(mr->VolStatus));
   }

   Dmsg2(100, "Set Medium %s: to VolStatus %s", mr->VolumeName, mr->VolStatus);

   /*
    * Update LastWritten Timestamp
    */
   mr->LastWritten = (utime_t)time(NULL);

   /*
    * VolBytes
    */
   mr->VolBytes += media->n_bytes;

   /*
    * also store file_marks
    */
   mr->VolFiles = media->file_mark_offset;
}

void ndmmedia_from_bareos_db_records(ndmmedia *media, MEDIA_DBR *mr, JOBMEDIA_DBR *jm)
{
   bstrncpy(media->label, mr->VolumeName, NDMMEDIA_LABEL_MAX - 1);
   media->valid_label = NDMP9_VALIDITY_VALID;

   media->slot_addr = mr->Slot;
   media->valid_slot = NDMP9_VALIDITY_VALID;

   media->n_bytes = mr->VolBytes;
   media->valid_n_bytes = NDMP9_VALIDITY_VALID;

   media->file_mark_offset = jm->StartBlock;
   media->valid_filemark = NDMP9_VALIDITY_VALID;

   media->n_bytes = jm->JobBytes;
   media->valid_n_bytes = NDMP9_VALIDITY_VALID;

}


bool store_ndmmedia_info_in_database(ndmmedia *media, JCR  *jcr)
{
   JOBMEDIA_DBR jm;
   MEDIA_DBR mr;

   memset(&jm, 0, sizeof(jm));
   memset(&mr, 0, sizeof(mr));

   /*
    * get media record by name
    */
   bstrncpy(mr.VolumeName, media->label, NDMMEDIA_LABEL_MAX);
   if (!jcr->db->get_media_record(jcr, &mr)){
      Jmsg(jcr, M_FATAL, 0, _("Catalog error getting Media record for Medium %s: %s"),
                              mr.VolumeName,  jcr->db->strerror());
      return false;
   }

   /*
    * map media info into db records
    */
   ndmmedia_to_bareos_db_records(media, &mr, &jm);


   /*
    * store db records
    */
   jm.MediaId = mr.MediaId;
   jm.JobId = jcr->JobId;
   if (!jcr->db->create_jobmedia_record(jcr, &jm)) {
      Jmsg(jcr, M_FATAL, 0, _("Catalog error creating JobMedia record. %s"), jcr->db->strerror());
      return false;
   }

   if (!jcr->db->update_media_record(jcr, &mr)){
      Jmsg(jcr, M_FATAL, 0, _("Catalog error updating Media record for Medium %s: %s"),
                              mr.VolumeName,  jcr->db->strerror());
      return false;
   }
   return true;
}



/*
 * get ndmmedia from database for certain job
 */
bool get_ndmmedia_info_from_database(ndm_media_table *media_tab, JCR  *jcr)
{

   JOBMEDIA_DBR jm;
   MEDIA_DBR mr;

   memset(&jm, 0, sizeof(jm));
   memset(&mr, 0, sizeof(mr));

   int VolCount;
   VOL_PARAMS *VolParams = NULL;
   bool retval = false;


   /*
    * Find restore JobId
    */
   JobId_t  restoreJobId;
   int status = 0;
   char *p = jcr->JobIds;

  /*
   *  TODO: what happens with multiple IDs?
   */
   if (! get_next_jobid_from_list(&p, &restoreJobId) ) {
      Jmsg(jcr, M_FATAL, 0, _("Error getting next jobid from list\n"));
   }
   if (restoreJobId == 0) {
      Jmsg(jcr, M_FATAL, 0, _("RestoreJobId is zero, cannot go on\n"));
   }
   /*
    * Get Media for certain job
    */
   VolCount = jcr->db->get_job_volume_parameters(jcr,
                                           restoreJobId,
                                           &VolParams);

   if (!VolCount) {
      Jmsg(jcr, M_ERROR, 0, _("Could not get Job Volume Parameters to "
               "create ndmmedia list. ERR=%s\n"), jcr->db->strerror());
      goto bail_out;
   }

   /*
    * create a ndmmedium for each volume
    */
   for (int i=0; i < VolCount; i++) {

      ndmmedia *media = ndma_store_media(media_tab, i);

      bstrncpy(media->label, VolParams[i].VolumeName, NDMMEDIA_LABEL_MAX);
      media->valid_label = NDMP9_VALIDITY_VALID;

      media->slot_addr = VolParams[i].Slot;
      media->valid_slot = NDMP9_VALIDITY_VALID;

      media->n_bytes = VolParams[i].JobBytes;
      media->valid_n_bytes = NDMP9_VALIDITY_VALID;

      // Vols[i].StartAddr = (((uint64_t)StartFile)<<32) | StartBlock;
      //VolParams[i].StartAddr >>= 32;
      media->file_mark_offset = VolParams[i].StartAddr;

      media->valid_filemark = NDMP9_VALIDITY_VALID;
#if 0

      Jmsg(jcr, M_INFO, 0, _("label           : %s\n"), media->label);
      Jmsg(jcr, M_INFO, 0, _("file_mark_offset: %d\n"), media->file_mark_offset);
      Jmsg(jcr, M_INFO, 0, _("Logical slot is : %d\n"), media->slot_addr);
      Jmsg(jcr, M_INFO, 0, _("index           : %d\n"), media->index);
      Jmsg(jcr, M_INFO, 0, _("n_bytes         : %u\n"), media->n_bytes);
      Jmsg(jcr, M_INFO, 0, _("begin_offset    : %u\n"), media->begin_offset);
      Jmsg(jcr, M_INFO, 0, _("end_offset      : %u\n"), media->end_offset);
#endif
   }
   retval = true;

bail_out:
   if (VolParams) {
      free(VolParams);
   }

   return retval;
}



#else

#endif /* HAVE_NDMP */
