#include <RendererCore/PCH.h>
#include <RendererCore/Pipeline/ExtractedRenderData.h>

#include <Foundation/Profiling/Profiling.h>

static ezProfilingId s_SortingProfilingId = ezProfilingSystem::CreateId("SortAndBatch");

ezExtractedRenderData::ezExtractedRenderData()
{
}

void ezExtractedRenderData::AddRenderData(const ezRenderData* pRenderData, ezRenderData::Category category, ezUInt32 uiRenderDataSortingKey)
{
  if (category >= m_DataPerCategory.GetCount())
  {
    m_DataPerCategory.SetCount(category + 1);
  }

  auto& sortableRenderData = m_DataPerCategory[category].m_SortableRenderData.ExpandAndGetRef();
  sortableRenderData.m_pRenderData = pRenderData;
  sortableRenderData.m_uiSortingKey = pRenderData->GetCategorySortingKey(category, uiRenderDataSortingKey, m_Camera);
}

void ezExtractedRenderData::SortAndBatch()
{
  EZ_PROFILE(s_SortingProfilingId);

  struct RenderDataComparer
  {
    EZ_FORCE_INLINE bool Less(const ezRenderDataBatch::SortableRenderData& a, const ezRenderDataBatch::SortableRenderData& b) const
    {
      return a.m_uiSortingKey < b.m_uiSortingKey;
    }
  };

  for (auto& dataPerCategory : m_DataPerCategory)
  {
    if (dataPerCategory.m_SortableRenderData.IsEmpty())
      continue;

    auto& data = dataPerCategory.m_SortableRenderData;

    // Sort
    data.Sort(RenderDataComparer());

    // Find batches
    ezUInt32 uiCurrentBatchId = data[0].m_pRenderData->m_uiBatchId;
    ezUInt32 uiCurrentBatchStartIndex = 0;
    const ezRTTI* pCurrentBatchType = data[0].m_pRenderData->GetDynamicRTTI();

    for (ezUInt32 i = 1; i < data.GetCount(); ++i)
    {
      auto pRenderData = data[i].m_pRenderData;

      if (pRenderData->m_uiBatchId != uiCurrentBatchId || pRenderData->GetDynamicRTTI() != pCurrentBatchType)
      {
        dataPerCategory.m_Batches.ExpandAndGetRef().m_Data = ezMakeArrayPtr(&data[uiCurrentBatchStartIndex], i - uiCurrentBatchStartIndex);

        uiCurrentBatchId = pRenderData->m_uiBatchId;
        uiCurrentBatchStartIndex = i;
        pCurrentBatchType = pRenderData->GetDynamicRTTI();
      }
    }

    dataPerCategory.m_Batches.ExpandAndGetRef().m_Data = ezMakeArrayPtr(&data[uiCurrentBatchStartIndex], data.GetCount() - uiCurrentBatchStartIndex);
  }
}

void ezExtractedRenderData::Clear()
{
  for (auto& dataPerCategory : m_DataPerCategory)
  {
    dataPerCategory.m_Batches.Clear();
    dataPerCategory.m_SortableRenderData.Clear();
  }

  // TODO: intelligent compact
}
