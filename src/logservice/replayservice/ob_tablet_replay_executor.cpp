/**
 * Copyright (c) 2023 OceanBase
 * OceanBase CE is licensed under Mulan PubL v2.
 * You can use this software according to the terms and conditions of the Mulan PubL v2.
 * You may obtain a copy of Mulan PubL v2 at:
 *          http://license.coscl.org.cn/MulanPubL-2.0
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
 * EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
 * MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
 * See the Mulan PubL v2 for more details.
 */

#include "logservice/replayservice/ob_tablet_replay_executor.h"
#include "storage/ls/ob_ls.h"
#include "storage/ls/ob_ls_get_mod.h"
#include "storage/tx_storage/ob_ls_service.h"

namespace oceanbase
{
namespace logservice
{

#ifdef CLOG_LOG_LIMIT
#undef CLOG_LOG_LIMIT
#endif

#define CLOG_LOG_LIMIT(level, args...)             \
  do                                               \
  {                                                \
    if (REACH_TIME_INTERVAL(1000 * 1000)) {        \
      CLOG_LOG(level, ##args);                     \
    }                                              \
  } while(0)


int ObTabletReplayExecutor::replay_check_restore_status(storage::ObTabletHandle &tablet_handle, const bool update_tx_data)
{
  int ret = OB_SUCCESS;
  ObTablet *tablet = tablet_handle.get_obj();
  ObTabletRestoreStatus::STATUS restore_status = ObTabletRestoreStatus::STATUS::RESTORE_STATUS_MAX;
  if (OB_ISNULL(tablet)) {
    ret = OB_ERR_UNEXPECTED;
    CLOG_LOG(WARN, "tablet is null", K(ret));
  } else if (OB_FAIL(tablet->get_restore_status(restore_status))) {
    CLOG_LOG(WARN, "failed to get tablet restore status", K(ret));
  } else if (ObTabletRestoreStatus::is_undefined(restore_status)) {
    // UNDEFINED tablet need replay.
    ret = OB_SUCCESS;
    CLOG_LOG_LIMIT(INFO, "tablet is UNDEFINED, but need replay", K(restore_status), K(update_tx_data));
  } else if (ObTabletRestoreStatus::is_pending(restore_status)) {
    ret = OB_EAGAIN;
    CLOG_LOG_LIMIT(WARN, "tablet is PENDING, need retry", K(ret), K(restore_status), K(update_tx_data));
  }

  return ret;
}


int ObTabletReplayExecutor::execute(const share::SCN &scn, const share::ObLSID &ls_id, const common::ObTabletID &tablet_id)
{
  MDS_TG(5_ms);
  int ret = OB_SUCCESS;
  storage::ObTabletHandle tablet_handle;
  bool can_skip_replay = false;
  ObTablet *tablet = nullptr;
  ObLSHandle ls_handle;
  ObLSService *ls_service = nullptr;
  if (OB_UNLIKELY(!is_inited_)) {
    ret = OB_NOT_INIT;
    CLOG_LOG(WARN, "replay executor not init", KR(ret), K_(is_inited));
  } else if (!scn.is_valid()) {
    ret = OB_INVALID_ARGUMENT;
    CLOG_LOG(WARN, "replay executor get invalid argument", KR(ret), K(scn), K(ls_id));
  } else if (OB_ISNULL(ls_service = MTL(ObLSService *))) {
    ret = OB_ERR_UNEXPECTED;
    CLOG_LOG(WARN, "ls service should not be null", K(ret), KP(ls_service));
  } else if (CLICK_FAIL(ls_service->get_ls(ls_id, ls_handle, ObLSGetMod::TABLET_MOD))) {
    CLOG_LOG(WARN, "fail to get log stream", KR(ret), K(ls_id));
  } else if (CLICK_FAIL(check_can_skip_replay_(ls_handle, scn, can_skip_replay))) {
    CLOG_LOG(WARN, "failed to check can skip reply", K(ret), K(scn), K(ls_id));
  } else if (can_skip_replay) {
    // do nothing
  } else if (CLICK_FAIL(replay_get_tablet_(ls_handle, tablet_id, scn, tablet_handle))) {
    if (OB_OBSOLETE_CLOG_NEED_SKIP == ret) {
      CLOG_LOG(INFO, "clog is already obsolete, should skip replay", K(ret), K(ls_id), K(scn));
      ret = OB_SUCCESS;
    } else if (OB_EAGAIN == ret) {
      CLOG_LOG_LIMIT(WARN, "need retry to get tablet", K(ret), K(ls_id), K(scn));
    } else {
      CLOG_LOG(WARN, "failed to get tablet", K(ret), K(ls_id), K(scn));
    }
  } else if (CLICK_FAIL(replay_check_restore_status_(tablet_handle))) {
    if (OB_NO_NEED_UPDATE == ret) {
      CLOG_LOG(WARN, "no need replay after check restore status, skip this log", K(ret), K(ls_id), K(scn));
    } else if (OB_EAGAIN == ret) {
      CLOG_LOG_LIMIT(WARN, "need retry after check restore status", K(ret), K(ls_id), K(scn));
    } else {
      CLOG_LOG(WARN, "failed to check restore status", K(ret), K(ls_id), K(scn));
    }
  } else if (CLICK_FAIL(check_can_skip_replay_to_mds_(scn, tablet_handle, can_skip_replay))) {
    CLOG_LOG(WARN, "failed to check can skip reply to mds", K(ret), K(ls_id), K(scn), K(tablet_handle));
  } else if (can_skip_replay) {
    //do nothing
  } else if (CLICK_FAIL(do_replay_(tablet_handle))) {
    if (OB_NO_NEED_UPDATE == ret) {
      CLOG_LOG(WARN, "no need replay, skip this log", K(ret), K(ls_id), K(scn));
    } else if (OB_EAGAIN == ret) {
      CLOG_LOG_LIMIT(WARN, "failed to replay, need retry", K(ret), K(ls_id), K(scn));
    } else {
      CLOG_LOG(WARN, "failed to replay", K(ret), K(ls_id), K(scn));
    }
  }

  return ret;
}

int ObTabletReplayExecutor::replay_get_tablet_(
    const storage::ObLSHandle &ls_handle,
    const common::ObTabletID &tablet_id,
    const share::SCN &scn,
    storage::ObTabletHandle &tablet_handle)
{
  int ret = OB_SUCCESS;
  ObLS *ls = nullptr;
  if (!scn.is_valid() || !tablet_id.is_valid()) {
    ret = OB_INVALID_ARGUMENT;
    CLOG_LOG(WARN, "check can skip replay to mds get invalid argument", K(ret), K(scn), K(tablet_id));
  } else if (OB_ISNULL(ls = ls_handle.get_ls())) {
    ret = OB_ERR_UNEXPECTED;
    CLOG_LOG(WARN, "log stream should not be NULL", KR(ret), K(scn));
  } else if (is_replay_update_tablet_status_()) {
    if (OB_FAIL(ls->replay_get_tablet_no_check(tablet_id, scn, tablet_handle))) {
      CLOG_LOG(WARN, "replay get table failed", KR(ret), "ls_id", ls->get_ls_id());
    }
  } else if (OB_FAIL(ls->replay_get_tablet(tablet_id, scn, tablet_handle))) {
    CLOG_LOG(WARN, "replay get table failed", KR(ret), "ls_id", ls->get_ls_id());
  }
  return ret;
}

int ObTabletReplayExecutor::replay_check_restore_status_(storage::ObTabletHandle &tablet_handle)
{
  const bool update_user_data = is_replay_update_tablet_status_();
  return ObTabletReplayExecutor::replay_check_restore_status(tablet_handle, update_user_data);
}

int ObTabletReplayExecutor::check_can_skip_replay_to_mds_(
    const share::SCN &scn,
    storage::ObTabletHandle &tablet_handle,
    bool &can_skip)
{
  int ret = OB_SUCCESS;
  ObTablet *tablet = nullptr;
  can_skip = false;

  if (!scn.is_valid()) {
    ret = OB_INVALID_ARGUMENT;
    CLOG_LOG(WARN, "check can skip replay to mds get invalid argument", K(ret), K(scn));
  } else if (!is_replay_update_mds_table_()) {
    can_skip = false;
  } else if (OB_ISNULL(tablet = tablet_handle.get_obj())) {
    ret = OB_ERR_UNEXPECTED;
    CLOG_LOG(WARN, "tablet should not be NULL", K(ret), KP(tablet));
  } else if (tablet->get_tablet_meta().mds_checkpoint_scn_ >= scn) {
    can_skip = true;
    CLOG_LOG(INFO, "skip replay to mds", KPC(tablet), K(scn));
  } else {
    can_skip = false;
  }
  return ret;
}

int ObTabletReplayExecutor::check_can_skip_replay_(
    const storage::ObLSHandle &ls_handle,
    const share::SCN &scn,
    bool &can_skip)
{
  int ret = OB_SUCCESS;
  can_skip = false;
  ObLS *ls = nullptr;
  share::SCN tablet_change_scn = share::SCN::min_scn();
  if (!is_replay_update_tablet_status_()) {
    can_skip = false;
  } else if (!scn.is_valid()) {
    ret = OB_INVALID_ARGUMENT;
    CLOG_LOG(WARN, "check can skip replay to mds get invalid argument", K(ret), K(scn));
  } else if (OB_ISNULL(ls = ls_handle.get_ls())) {
    ret = OB_ERR_UNEXPECTED;
    CLOG_LOG(WARN, "log stream should not be NULL", KR(ret), K(scn));
  } else if (FALSE_IT(tablet_change_scn = ls->get_tablet_change_checkpoint_scn())) {
  } else if (scn <= tablet_change_scn) {
    can_skip = true;
    CLOG_LOG(INFO, "can skip replay", "ls_id", ls->get_ls_id(), K(tablet_change_scn), K(scn));
  }

  return ret;
}

int ObTabletReplayExecutor::replay_to_mds_table_(
    storage::ObTabletHandle &tablet_handle,
    const ObTabletCreateDeleteMdsUserData &mds,
    storage::mds::MdsCtx &ctx,
    const share::SCN &scn,
    const bool for_old_mds)
{
  int ret = OB_SUCCESS;
  storage::ObTablet *tablet = tablet_handle.get_obj();
  if (!is_replay_update_mds_table_()) {
    ret = OB_ERR_UNEXPECTED;
    CLOG_LOG(WARN, "replay log do not update mds table, cannot replay to mds table", K(ret), K(tablet_handle));
  } else if (OB_ISNULL(tablet)) {
    ret = OB_ERR_UNEXPECTED;
    CLOG_LOG(WARN, "tablet should not be NULL", KR(ret));
  } else if (tablet->is_ls_inner_tablet()) {
    ret = OB_NOT_SUPPORTED;
    CLOG_LOG(WARN, "inner tablets have no mds table", KR(ret));
  } else {
    ObLSService *ls_svr = MTL(ObLSService*);
    ObLSHandle ls_handle;
    ObLS *ls = nullptr;
    const share::ObLSID &ls_id = tablet->get_tablet_meta().ls_id_;
    const common::ObTabletID &tablet_id = tablet->get_tablet_meta().tablet_id_;
    if (OB_FAIL(ls_svr->get_ls(ls_id, ls_handle, ObLSGetMod::TABLET_MOD))) {
      CLOG_LOG(WARN, "failed to get ls", K(ret), K(ls_id));
    } else if (OB_ISNULL(ls = ls_handle.get_ls())) {
      ret = OB_ERR_UNEXPECTED;
      CLOG_LOG(WARN, "ls is null", K(ret), K(ls_id), KP(ls));
    } else if (for_old_mds) {
      if (OB_FAIL(ls->get_tablet_svr()->set_tablet_status(tablet_id, mds, ctx))) {
        CLOG_LOG(WARN, "failed to set mds data", K(ret), K(ls_id), K(tablet_id), K(scn), K(mds));
      }
    } else {
      if (OB_FAIL(ls->get_tablet_svr()->replay_set_tablet_status(tablet_id, scn, mds, ctx))) {
        CLOG_LOG(WARN, "failed to replay set tablet status", K(ret), K(ls_id), K(tablet_id), K(scn), K(mds));
      }
    }
  }
  return ret;
}

int ObTabletReplayExecutor::replay_to_mds_table_(
    storage::ObTabletHandle &tablet_handle,
    const ObTabletBindingMdsUserData &mds,
    storage::mds::MdsCtx &ctx,
    const share::SCN &scn,
    const bool for_old_mds)
{
  int ret = OB_SUCCESS;
  storage::ObTablet *tablet = tablet_handle.get_obj();
  if (!is_replay_update_mds_table_()) {
    ret = OB_ERR_UNEXPECTED;
    CLOG_LOG(WARN, "replay log do not update mds table, cannot replay to mds table", K(ret), K(tablet_handle));
  } else if (OB_ISNULL(tablet)) {
    ret = OB_ERR_UNEXPECTED;
    CLOG_LOG(WARN, "tablet should not be NULL", KR(ret));
  } else if (tablet->is_ls_inner_tablet()) {
    ret = OB_NOT_SUPPORTED;
    CLOG_LOG(WARN, "inner tablets have no mds table", KR(ret));
  } else {
    ObLSService *ls_svr = MTL(ObLSService*);
    ObLSHandle ls_handle;
    ObLS *ls = nullptr;
    const share::ObLSID &ls_id = tablet->get_tablet_meta().ls_id_;
    const common::ObTabletID &tablet_id = tablet->get_tablet_meta().tablet_id_;
    if (OB_FAIL(ls_svr->get_ls(ls_id, ls_handle, ObLSGetMod::TABLET_MOD))) {
      CLOG_LOG(WARN, "failed to get ls", K(ret), K(ls_id));
    } else if (OB_ISNULL(ls = ls_handle.get_ls())) {
      ret = OB_ERR_UNEXPECTED;
      CLOG_LOG(WARN, "ls is null", K(ret), K(ls_id), KP(ls));
    } else if (for_old_mds) {
      if (OB_FAIL(ls->get_tablet_svr()->set_ddl_info(tablet_id, mds, ctx, 0))) {
        CLOG_LOG(WARN, "failed to save tablet binding info", K(ret), K(ls_id), K(tablet_id), K(scn), K(mds));
      }
    } else {
      if (OB_FAIL(ls->get_tablet_svr()->replay_set_ddl_info(tablet_id, scn, mds, ctx))) {
        CLOG_LOG(WARN, "failed to replay set ddl info", K(ret), K(ls_id), K(tablet_id), K(scn), K(mds));
      }
    }
  }
  return ret;
}

}
}
