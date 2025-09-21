#pragma once

#include <QWidget>

class QLineEdit;
class QPushButton;
class QLabel;
class QCloseEvent;
class QEvent;

namespace LlamaCpp {

class SearchToolbar : public QWidget
{
    Q_OBJECT
public:
    explicit SearchToolbar(QWidget *parent = nullptr);

    void setIndexLabel(const QString &label);
signals:
    void onCloseEvent();
    void onNextSearchClicked(bool checked = false);
    void onPrevSearchClicked(bool checked = false);
    void onSearchTextChanged(const QString &text);

protected:
    void closeEvent(QCloseEvent *event) override;
    bool eventFilter(QObject *obj, QEvent *event) override;
    void applyStyleSheet();

private:
    QLineEdit *m_searchEdit{nullptr};
    QLabel *m_indexLabel;
    QPushButton *m_prevSearchBtn{nullptr};
    QPushButton *m_nextSearchBtn{nullptr};
};

} // namespace LlamaCpp
