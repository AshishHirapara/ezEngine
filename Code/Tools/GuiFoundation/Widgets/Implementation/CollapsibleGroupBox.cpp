#include <PCH.h>
#include <GuiFoundation/Widgets/CollapsibleGroupBox.moc.h>
#include <GuiFoundation/UIServices/UIServices.moc.h>
#include <QPainter>
#include <QMouseEvent>
#include <QScrollArea>

ezQtCollapsibleGroupBox::ezQtCollapsibleGroupBox(QWidget* pParent) : ezQtGroupBoxBase(pParent), m_bCollapsed(false)
{
  setupUi(this);

  Header->installEventFilter(this);
}

void ezQtCollapsibleGroupBox::SetTitle(const char* szTitle)
{
  ezQtGroupBoxBase::SetTitle(szTitle);
  update();
}

void ezQtCollapsibleGroupBox::SetIcon(const QIcon& icon)
{
  ezQtGroupBoxBase::SetIcon(icon);
  update();
}

void ezQtCollapsibleGroupBox::SetFillColor(const QColor& color)
{
  ezQtGroupBoxBase::SetFillColor(color);
  update();
}

void ezQtCollapsibleGroupBox::SetCollapseState(bool bCollapsed)
{
  if (bCollapsed == m_bCollapsed)
    return;

  ezQtScopedUpdatesDisabled sud(this);

  m_bCollapsed = bCollapsed;
  Content->setVisible(!bCollapsed);

  // Force re-layout of parent hierarchy to prevent flicker.
  QWidget* pCur = this;
  while (pCur != nullptr && qobject_cast<QScrollArea*>(pCur) == nullptr)
  {
    pCur->updateGeometry();
    pCur = pCur->parentWidget();
  }

  emit CollapseStateChanged(bCollapsed);
}

bool ezQtCollapsibleGroupBox::GetCollapseState() const
{
  return m_bCollapsed;
}

QWidget* ezQtCollapsibleGroupBox::GetContent()
{
  return Content;
}

QWidget* ezQtCollapsibleGroupBox::GetHeader()
{
  return Header;
}

bool ezQtCollapsibleGroupBox::eventFilter(QObject* object, QEvent* event)
{
  if (event->type() == QEvent::Type::MouseButtonPress || event->type() == QEvent::Type::MouseButtonDblClick)
  {
    QMouseEvent* pMouseEvent = static_cast<QMouseEvent*>(event);

    if (pMouseEvent->button() == Qt::MouseButton::LeftButton)
    {
      SetCollapseState(!m_bCollapsed);
      return true;
    }
  }

  return false;
}

void ezQtCollapsibleGroupBox::paintEvent(QPaintEvent* event)
{
  const QPalette& pal = palette();
  QWidget::paintEvent(event);

  QPainter p(this);
  p.setRenderHint(QPainter::Antialiasing);
  QRect wr = contentsRect();
  QRect hr = Header->contentsRect();
  hr.moveTopLeft(Header->pos());

  QRect cr = wr;
  cr.setTop(hr.height());
  cr.adjust(Rounding / 2, 0, 0, -Rounding / 2);

  if (m_FillColor.isValid())
  {
    QRectF wrAdjusted = wr;
    wrAdjusted.adjust(0.5, 0.5, Rounding, -0.5);
    QPainterPath oPath;
    oPath.addRoundedRect(wrAdjusted, Rounding, Rounding);
    p.fillPath(oPath, pal.alternateBase());

    QRectF crAdjusted = cr;
    crAdjusted.adjust(0.5, 0.5, Rounding, -0.5);
    QPainterPath path;
    path.addRoundedRect(crAdjusted, Rounding, Rounding);
    p.fillPath(path, pal.window());
  }

  DrawHeader(p, hr, m_sTitle, m_Icon, true);
}
