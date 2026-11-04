# @file    IntegrationTimeouts.cmake
# @brief   逐次ワーカー統合検証の実測に基づく個別の実行上限。
# @author  Hasegawa Jin
# @date    2026-10-30

# @note 1 worker の 4 bake は Release 単独実行で約 34 秒かかる。検証内容と完了条件は維持する。
# @note CTest が読み込んだ discovery 一覧で登録を確認し、未ビルドのスイートには適用しない。
list(FIND FBZZTestsEngineAuto_TESTS "NavMeshBakeConcurrencyTest.CompletesSeveralBakesWithOneWorker" navMeshBakeTestIndex)
if(NOT navMeshBakeTestIndex EQUAL -1)
    set_tests_properties(NavMeshBakeConcurrencyTest.CompletesSeveralBakesWithOneWorker PROPERTIES TIMEOUT 90)
endif()
