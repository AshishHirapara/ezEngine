#include <PCH.h>
#include <EditorFramework/ContainerWindow/ContainerWindow.moc.h>
#include <EditorFramework/DocumentWindow/DocumentWindow.moc.h>
#include <EditorFramework/EditorApp.moc.h>
#include <EditorFramework/Settings/SettingsTab.moc.h>
#include <ToolsFoundation/Project/EditorProject.h>
#include <ToolsFoundation/Document/DocumentManager.h>
#include <GuiFoundation/UIServices/UIServices.moc.h>
#include <Foundation/IO/OSFile.h>
#include <GuiFoundation/Action/ActionMapManager.h>
#include <GuiFoundation/Action/DocumentActions.h>
#include <GuiFoundation/ActionViews/MenuActionMapView.moc.h>
#include <QSettings>
#include <QMenu>
#include <QMenuBar>
#include <QTimer>
#include <QPushButton>
#include <QToolButton>
#include <QMessageBox>
#include <QFileDialog>
#include <QProcess>
#include <QCloseEvent>

ezDynamicArray<ezContainerWindow*> ezContainerWindow::s_AllContainerWindows;

ezContainerWindow::ezContainerWindow()
{
  ezActionMapManager::RegisterActionMap("RootMenu");
  ezDocumentActions::MapActions("RootMenu", ezHashedString());

  setObjectName(QLatin1String(GetUniqueName())); // todo
  setWindowIcon(QIcon(QLatin1String(":/Icons/Icons/ezEditor16.png")));

  QTimer::singleShot(0, this, SLOT(SlotRestoreLayout()));

  s_AllContainerWindows.PushBack(this);

  m_pActionCurrentTabSave       = new QAction("Save", this);
  m_pActionCurrentTabSaveAll    = new QAction("Save All", this);
  m_pActionCurrentTabClose      = new QAction("Close", this);
  m_pActionCurrentTabOpenFolder = new QAction("Open Containing Folder", this);

  EZ_VERIFY(connect(m_pActionCurrentTabSave,       SIGNAL(triggered()), this, SLOT(SlotCurrentTabSave()))        != nullptr, "signal/slot connection failed");
  EZ_VERIFY(connect(m_pActionCurrentTabSaveAll,    SIGNAL(triggered()), this, SLOT(SlotCurrentTabSaveAll()))     != nullptr, "signal/slot connection failed");
  EZ_VERIFY(connect(m_pActionCurrentTabClose,      SIGNAL(triggered()), this, SLOT(SlotCurrentTabClose()))       != nullptr, "signal/slot connection failed");
  EZ_VERIFY(connect(m_pActionCurrentTabOpenFolder, SIGNAL(triggered()), this, SLOT(SlotCurrentTabOpenFolder()))  != nullptr, "signal/slot connection failed");

  ezDocumentWindow::s_Events.AddEventHandler(ezMakeDelegate(&ezContainerWindow::DocumentWindowEventHandler, this));
  ezToolsProject::s_Events.AddEventHandler(ezMakeDelegate(&ezContainerWindow::ProjectEventHandler, this));

  UpdateWindowTitle();
}

ezContainerWindow::~ezContainerWindow()
{
  s_AllContainerWindows.Remove(this);

  ezDocumentWindow::s_Events.RemoveEventHandler(ezMakeDelegate(&ezContainerWindow::DocumentWindowEventHandler, this));
  ezToolsProject::s_Events.RemoveEventHandler(ezMakeDelegate(&ezContainerWindow::ProjectEventHandler, this));
}

QTabWidget* ezContainerWindow::GetTabWidget() const
{
  QTabWidget* pTabs = (QTabWidget*) centralWidget();
  EZ_ASSERT_DEV(pTabs != nullptr, "The central widget is NULL");

  return pTabs;
}

void ezContainerWindow::UpdateWindowTitle()
{
  ezStringBuilder sTitle;

  if (ezToolsProject::IsProjectOpen())
  {
    sTitle = ezPathUtils::GetFileName(ezToolsProject::GetInstance()->GetProjectPath());
    sTitle.Append(" - ");
  }
  
  sTitle.Append(ezEditorApp::GetInstance()->GetApplicationName());

  setWindowTitle(QString::fromUtf8(sTitle.GetData()));
}

void ezContainerWindow::SlotRestoreLayout()
{
  RestoreWindowLayout();
}

void ezContainerWindow::closeEvent(QCloseEvent* e)
{
  if (!ezToolsProject::CanCloseProject())
  {
    e->setAccepted(false);
    return;
  }

  ezToolsProject::CloseProject();
  SaveWindowLayout();
}

void ezContainerWindow::SaveWindowLayout()
{
  for (ezUInt32 i = 0; i < m_DocumentWindows.GetCount(); ++i)
    m_DocumentWindows[i]->SaveWindowLayout();

  QTabWidget* pTabs = GetTabWidget();

  if (pTabs->currentWidget())
  {
    ezDocumentWindow* pDoc = (ezDocumentWindow*) pTabs->currentWidget();
    pDoc->SaveWindowLayout();
  }

  const bool bMaximized = isMaximized();

  if (bMaximized)
    showNormal();

  ezStringBuilder sGroup;
  sGroup.Format("ContainerWnd_%s", GetUniqueName());

  QSettings Settings;
  Settings.beginGroup(QString::fromUtf8(sGroup));
  {
    Settings.setValue("WindowGeometry", saveGeometry());
    Settings.setValue("WindowState", saveState());
    Settings.setValue("IsMaximized", bMaximized);
    Settings.setValue("WindowPosition", pos());

    if (!bMaximized)
      Settings.setValue("WindowSize", size());
  }
  Settings.endGroup();
}

void ezContainerWindow::RestoreWindowLayout()
{
  ezStringBuilder sGroup;
  sGroup.Format("ContainerWnd_%s", GetUniqueName());

  QSettings Settings;
  Settings.beginGroup(QString::fromUtf8(sGroup));
  {
    restoreGeometry(Settings.value("WindowGeometry", saveGeometry()).toByteArray());

    move(Settings.value("WindowPosition", pos()).toPoint());
    resize(Settings.value("WindowSize", size()).toSize());

    if (Settings.value("IsMaximized", isMaximized()).toBool())
      showMaximized();

    restoreState(Settings.value("WindowState", saveState()).toByteArray());
  }
  Settings.endGroup();
}

void ezContainerWindow::SetupDocumentTabArea()
{
  if (centralWidget() != nullptr)
    return;

  QTabWidget* pTabs = new QTabWidget(this);
  pTabs->setObjectName("DocumentTabs");
  pTabs->setTabsClosable(true);
  pTabs->setMovable(true);
  pTabs->setTabShape(QTabWidget::TabShape::Rounded);
  pTabs->setDocumentMode(true);
  pTabs->tabBar()->setContextMenuPolicy(Qt::ContextMenuPolicy::CustomContextMenu);

  EZ_VERIFY(connect(pTabs->tabBar(), SIGNAL(customContextMenuRequested(const QPoint&)), this, SLOT(SlotTabsContextMenuRequested(const QPoint&))) != nullptr, "signal/slot connection failed");

  EZ_VERIFY(connect(pTabs, SIGNAL(tabCloseRequested(int)), this, SLOT(SlotDocumentTabCloseRequested(int))) != nullptr, "signal/slot connection failed");
  EZ_VERIFY(connect(pTabs, SIGNAL(currentChanged(int)), this, SLOT(SlotDocumentTabCurrentChanged(int))) != nullptr, "signal/slot connection failed");

  setCentralWidget(pTabs);
}

void ezContainerWindow::UpdateWindowDecoration(ezDocumentWindow* pDocWindow)
{
  const ezInt32 iListIndex = m_DocumentWindows.IndexOf(pDocWindow);

  if (iListIndex == ezInvalidIndex)
    return;

  QTabWidget* pTabs = GetTabWidget();

  int iTabIndex = pTabs->indexOf(pDocWindow);
  EZ_ASSERT_DEV(iTabIndex >= 0, "Invalid document window to close");

  pTabs->setTabToolTip(iTabIndex, QString::fromUtf8(pDocWindow->GetDisplayName().GetData()));
  pTabs->setTabText(iTabIndex, QString::fromUtf8(pDocWindow->GetDisplayNameShort().GetData()));
  pTabs->setTabIcon(iTabIndex, QIcon(QString::fromUtf8(pDocWindow->GetTypeIcon().GetData())));
}

void ezContainerWindow::RemoveDocumentWindowFromContainer(ezDocumentWindow* pDocWindow)
{
  const ezInt32 iListIndex = m_DocumentWindows.IndexOf(pDocWindow);

  if (iListIndex == ezInvalidIndex)
    return;

  QTabWidget* pTabs = GetTabWidget();

  int iTabIndex = pTabs->indexOf(pDocWindow);
  EZ_ASSERT_DEV(iTabIndex >= 0, "Invalid document window to close");

  pTabs->removeTab(iTabIndex);

  m_DocumentWindows.RemoveAtSwap(iListIndex);

  pDocWindow->m_pContainerWindow = nullptr;
}

void ezContainerWindow::MoveDocumentWindowToContainer(ezDocumentWindow* pDocWindow)
{
  if (m_DocumentWindows.IndexOf(pDocWindow) != ezInvalidIndex)
    return;

  if (pDocWindow->m_pContainerWindow != nullptr)
    pDocWindow->m_pContainerWindow->RemoveDocumentWindowFromContainer(pDocWindow);

  EZ_ASSERT_DEV(pDocWindow->m_pContainerWindow == nullptr, "Implementation error");

  SetupDocumentTabArea();

  m_DocumentWindows.PushBack(pDocWindow);
  pDocWindow->m_pContainerWindow = this;

  QTabWidget* pTabs = GetTabWidget();
  int iTab = pTabs->addTab(pDocWindow, QString::fromUtf8(pDocWindow->GetDisplayNameShort()));

  UpdateWindowDecoration(pDocWindow);
}

ezResult ezContainerWindow::EnsureVisible(ezDocumentWindow* pDocWindow)
{
  if (m_DocumentWindows.IndexOf(pDocWindow) == ezInvalidIndex)
    return EZ_FAILURE;

  QTabWidget* pTabs = GetTabWidget();

  pTabs->setCurrentWidget(pDocWindow);
  return EZ_SUCCESS;
}

ezResult ezContainerWindow::EnsureVisible(ezDocumentBase* pDocument)
{
  for (auto doc : m_DocumentWindows)
  {
    if (doc->GetDocument() == pDocument)
      return EnsureVisible(doc);
  }

  return EZ_FAILURE;
}

ezResult ezContainerWindow::EnsureVisibleAnyContainer(ezDocumentBase* pDocument)
{
  for (auto cont : s_AllContainerWindows)
  {
    if (cont->EnsureVisible(pDocument).Succeeded())
      return EZ_SUCCESS;
  }

  return EZ_FAILURE;
}

void ezContainerWindow::SlotDocumentTabCloseRequested(int index)
{
  QTabWidget* pTabs = GetTabWidget();

  ezDocumentWindow* pDocWindow = (ezDocumentWindow*) pTabs->widget(index);

  // Prevent closing the only tab
  // this is not necessary, since a new settings tab would be created, but it is a bit cleaner
  if (m_DocumentWindows.GetCount() == 1 && pDocWindow == ezSettingsTab::GetInstance())
    return;

  if (!pDocWindow->CanCloseWindow())
    return;

  pDocWindow->CloseDocumentWindow();
}

void ezContainerWindow::SlotDocumentTabCurrentChanged(int index)
{
  QTabWidget* pTabs = GetTabWidget();

  ezDocumentWindow* pNowCurrentWindow = nullptr; 

  if (index >= 0)
    pNowCurrentWindow = (ezDocumentWindow*) pTabs->widget(index);

  for (ezDocumentWindow* pWnd : m_DocumentWindows)
  {
    if (pNowCurrentWindow == pWnd)
      continue;

    pWnd->SetVisibleInContainer(false);
  }

  if (pNowCurrentWindow)
    pNowCurrentWindow->SetVisibleInContainer(true);
}

void ezContainerWindow::DocumentWindowEventHandler(const ezDocumentWindow::Event& e)
{
  switch (e.m_Type)
  {
  case ezDocumentWindow::Event::Type::WindowClosed:
    RemoveDocumentWindowFromContainer(e.m_pWindow);

    if (m_DocumentWindows.IsEmpty())
      ShowSettingsDocument();

    break;
  case ezDocumentWindow::Event::Type::WindowDecorationChanged:
    UpdateWindowDecoration(e.m_pWindow);
    break;
  }
}

void ezContainerWindow::ProjectEventHandler(const ezToolsProject::Event& e)
{
  switch (e.m_Type)
  {
  case ezToolsProject::Event::Type::ProjectOpened:
  case ezToolsProject::Event::Type::ProjectClosed:
    UpdateWindowTitle();
    break;
  }
}

void ezContainerWindow::ShowSettingsDocument()
{
  ezSettingsTab* pSettingsTab = ezSettingsTab::GetInstance();

  if (pSettingsTab == nullptr)
  {
    pSettingsTab = new ezSettingsTab();

    ezEditorApp::GetInstance()->AddDocumentWindow(pSettingsTab);
  }

  pSettingsTab->EnsureVisible();
}

ezString ezContainerWindow::BuildDocumentTypeFileFilter(bool bForCreation)
{
  ezStringBuilder sAllFilters;
  const char* sepsep = "";

  if (!bForCreation)
  {
    sAllFilters = "All Files (*.*)";
    sepsep = ";;";
  }

  for (ezDocumentManagerBase* pMan : ezDocumentManagerBase::GetAllDocumentManagers())
  {
    ezHybridArray<ezDocumentTypeDescriptor, 4> Types;
    pMan->GetSupportedDocumentTypes(Types);

    for (const ezDocumentTypeDescriptor& desc : Types)
    {
      if (bForCreation && !desc.m_bCanCreate)
        continue;

      if (desc.m_sFileExtensions.IsEmpty())
        continue;

      sAllFilters.Append(sepsep, desc.m_sDocumentTypeName, " (");
      sepsep = ";;";

      const char* sep = "";

      for (const ezString& ext : desc.m_sFileExtensions)
      {
        sAllFilters.Append(sep, "*.", ext);
        sep = "; ";
      }

      sAllFilters.Append(")");

      desc.m_sDocumentTypeName;
    }
  }

  return sAllFilters;
}

ezResult ezContainerWindow::FindDocumentTypeFromPath(const char* szPath, bool bForCreation, ezDocumentManagerBase*& out_pTypeManager, ezDocumentTypeDescriptor& out_TypeDesc)
{
  const ezString sFileExt = ezPathUtils::GetFileExtension(szPath);

  out_pTypeManager = nullptr;

  for (ezDocumentManagerBase* pMan : ezDocumentManagerBase::GetAllDocumentManagers())
  {
    ezHybridArray<ezDocumentTypeDescriptor, 4> Types;
    pMan->GetSupportedDocumentTypes(Types);

    for (const ezDocumentTypeDescriptor& desc : Types)
    {
      if (bForCreation && !desc.m_bCanCreate)
        continue;

      for (const ezString& ext : desc.m_sFileExtensions)
      {
        if (ext.IsEqual_NoCase(sFileExt))
        {
          out_pTypeManager = pMan;
          out_TypeDesc = desc;
          return EZ_SUCCESS;
        }
      }
    }
  }

  return EZ_FAILURE;
}

void ezContainerWindow::CreateOrOpenDocument(bool bCreate)
{
  const ezString sAllFilters = BuildDocumentTypeFileFilter(bCreate);

  if (sAllFilters.IsEmpty())
  {
    ezUIServices::MessageBoxInformation("No file types are currently known. Load plugins to add file types.");
    return;
  }

  static QString sSelectedExt;
  static QString sDir = ezOSFile::GetApplicationDirectory();
  ezString sFile;
  
  if (bCreate)
    sFile = QFileDialog::getSaveFileName(QApplication::activeWindow(), QLatin1String("Create Document"), sDir, QString::fromUtf8(sAllFilters.GetData()), &sSelectedExt).toUtf8().data();
  else
    sFile = QFileDialog::getOpenFileName(QApplication::activeWindow(), QLatin1String("Open Document"), sDir, QString::fromUtf8(sAllFilters.GetData()), &sSelectedExt).toUtf8().data();

  if (sFile.IsEmpty())
    return;

  sDir = ezString(ezPathUtils::GetFileDirectory(sFile)).GetData();

  ezDocumentManagerBase* pManToCreate = nullptr;
  ezDocumentTypeDescriptor DescToCreate;

  if (FindDocumentTypeFromPath(sFile, bCreate, pManToCreate, DescToCreate).Succeeded())
  {
    sSelectedExt = DescToCreate.m_sDocumentTypeName;
  }

  CreateOrOpenDocument(bCreate, sFile);
}

void ezContainerWindow::CreateOrOpenDocument(bool bCreate, const char* szFile)
{
  ezDocumentManagerBase* pManToCreate = nullptr;
  ezDocumentTypeDescriptor DescToCreate;

  if (FindDocumentTypeFromPath(szFile, bCreate, pManToCreate, DescToCreate).Failed())
  {
    ezUIServices::MessageBoxWarning("The selected file extension is not registered with any known type.");
    return;
  }

  // does the same document already exist and is open ?
  ezDocumentBase* pDocument = pManToCreate->GetDocumentByPath(szFile);

  if (!pDocument)
  {
    ezStatus res;
  
    if (bCreate)
      res = pManToCreate->CreateDocument(DescToCreate.m_sDocumentTypeName, szFile, pDocument);
    else
    {
      res = pManToCreate->CanOpenDocument(szFile);

      if (res.m_Result.Succeeded())
      {
        res = pManToCreate->OpenDocument(DescToCreate.m_sDocumentTypeName, szFile, pDocument);
      }
    }

    if (res.m_Result.Failed())
    {
      ezStringBuilder s;
      s.Format("Failed to open document: \n'%s'", szFile);

      ezUIServices::MessageBoxStatus(res, s);
      return;
    }

    EZ_ASSERT_DEV(pDocument != nullptr, "Creation of document type '%s' succeeded, but returned pointer is NULL", DescToCreate.m_sDocumentTypeName.GetData());
  }
  else
  {
    if (bCreate)
    {
      ezUIServices::MessageBoxInformation("The selected document is already open. You need to close the document before you can re-create it.");
    }
  }

  ezContainerWindow::EnsureVisibleAnyContainer(pDocument);
}

void ezContainerWindow::CreateOrOpenProject(bool bCreate)
{
  static QString sDir = ezOSFile::GetApplicationDirectory();
  ezString sFile;

  const char* szFilter = "ezEditor Project (*.project)";
  
  if (bCreate)
    sFile = QFileDialog::getSaveFileName(QApplication::activeWindow(), QLatin1String("Create Project"), sDir, QLatin1String(szFilter)).toUtf8().data();
  else
    sFile = QFileDialog::getOpenFileName(QApplication::activeWindow(), QLatin1String("Open Project"), sDir, QLatin1String(szFilter)).toUtf8().data();

  if (sFile.IsEmpty())
    return;

  sDir = ezString(ezPathUtils::GetFileDirectory(sFile)).GetData();

  CreateOrOpenProject(bCreate, sFile);
}

void ezContainerWindow::CreateOrOpenProject(bool bCreate, const char* szFile)
{
  if (ezToolsProject::IsProjectOpen() && ezToolsProject::GetInstance()->GetProjectPath() == szFile)
  {
    ezUIServices::MessageBoxInformation("The selected project is already open");
    return;
  }

  if (!ezToolsProject::CanCloseProject())
    return;

  ezStatus res;
  if (bCreate)
    res = ezToolsProject::CreateProject(szFile);
  else
    res = ezToolsProject::OpenProject(szFile);

  if (res.m_Result.Failed())
  {
    ezStringBuilder s;
    s.Format("Failed to open project:\n'%s'", szFile);

    ezUIServices::MessageBoxStatus(res, s);
    return;
  }
}

void ezContainerWindow::SlotTabsContextMenuRequested(const QPoint& pos)
{
  QTabWidget* pTabs = GetTabWidget();

  int iTab = pTabs->tabBar()->tabAt(pos);

  if (iTab < 0)
  {
    // general menu
  }
  else
  {
    pTabs->setCurrentIndex(iTab);

    if (!pTabs->currentWidget())
      return;

    ezDocumentWindow* pDoc = (ezDocumentWindow*) pTabs->currentWidget();

    if (pDoc->GetDocument() == nullptr)
      return;

    ezMenuActionMapView menu;

    ezActionContext context;
    context.m_sMapping = "RootMenu";
    context.m_pDocument = pDoc->GetDocument();
    menu.SetActionContext(context);

    menu.exec(pTabs->tabBar()->mapToGlobal(pos));

    /*
    QMenu m;

    if (pDoc->GetDocument() && !pDoc->GetDocument()->IsReadOnly())
      m.addAction(m_pActionCurrentTabSave);

    m.addAction(m_pActionCurrentTabSaveAll);
    m.addAction(m_pActionCurrentTabClose);
    m.addSeparator();

    //if (pDoc->GetDocument())
      m.addAction(m_pActionCurrentTabOpenFolder);

    m.exec(pTabs->tabBar()->mapToGlobal(pos));
    */
  }
}

void ezContainerWindow::SlotCurrentTabSave()
{
  QTabWidget* pTabs = GetTabWidget();

  if (!pTabs->currentWidget())
    return;

  ezDocumentWindow* pDoc = (ezDocumentWindow*) pTabs->currentWidget();
  pDoc->SaveDocument();
}

void ezContainerWindow::SlotCurrentTabSaveAll()
{
  for (ezContainerWindow* pWnd : s_AllContainerWindows)
  {
    for (ezDocumentWindow* pDocWnd : pWnd->m_DocumentWindows)
    {
      if (pDocWnd->SaveDocument().m_Result.Failed())
        break;
    }
  }
}

void ezContainerWindow::SlotCurrentTabClose()
{
  QTabWidget* pTabs = GetTabWidget();

  SlotDocumentTabCloseRequested(pTabs->currentIndex());
}

void ezContainerWindow::SlotCurrentTabOpenFolder()
{
  QTabWidget* pTabs = GetTabWidget();

  if (!pTabs->currentWidget())
    return;

  ezDocumentWindow* pDocWnd = (ezDocumentWindow*) pTabs->currentWidget();

  ezDocumentBase* pDocument = pDocWnd->GetDocument();
  ezString sPath;

  if (!pDocument)
  {
    if (ezToolsProject::IsProjectOpen())
      sPath = ezToolsProject::GetInstance()->GetProjectPath();
    else
      sPath = ezOSFile::GetApplicationDirectory();
  }
  else
    sPath = pDocument->GetDocumentPath();

  QStringList args;
  args << "/select," << QDir::toNativeSeparators(sPath.GetData());
  QProcess::startDetached("explorer", args);
}

