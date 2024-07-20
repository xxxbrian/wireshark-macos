/* display_filter_expression_dialog.cpp
 *
 * Wireshark - Network traffic analyzer
 * By Gerald Combs <gerald@wireshark.org>
 * Copyright 1998 Gerald Combs
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <algorithm>

#include "display_filter_expression_dialog.h"
#include <ui_display_filter_expression_dialog.h>

#include <epan/proto.h>
#include <epan/range.h>
#include <epan/tfs.h>
#include <epan/value_string.h>

#include <wsutil/utf8_entities.h>

#include <ui/qt/utils/qt_ui_utils.h>
#include "main_application.h"

#include <ui/qt/utils/variant_pointer.h>

#include <QPushButton>
#include <QDialogButtonBox>
#include <QListWidgetItem>
#include <QTreeWidgetItem>
#include <QRegularExpression>
#include <QtConcurrent>

// To do:
// - Speed up search.

enum {
    proto_type_ = 1000,
    field_type_
};

enum {
    present_op_ = 1000,
    any_eq_op_,
    all_eq_op_,
    any_ne_op_,
    all_ne_op_,
    gt_op_,
    lt_op_,
    ge_op_,
    le_op_,
    contains_op_,
    matches_op_,
    in_op_
};

static inline bool compareTreeWidgetItems(const QTreeWidgetItem *it1, const QTreeWidgetItem *it2)
{
    return *it1 < *it2;
}

#ifdef DISPLAY_FILTER_EXPRESSION_DIALOG_USE_QPROMISE
static void generateProtocolTreeItems(QPromise<QTreeWidgetItem *> &promise)
{
    QList<QTreeWidgetItem *> proto_list;
    QList<QTreeWidgetItem *> *ptr_proto_list = &proto_list;
#else
static QList<QTreeWidgetItem *> *generateProtocolTreeItems()
{
    QList<QTreeWidgetItem *> *ptr_proto_list = new QList<QTreeWidgetItem *>();
#endif

    void *proto_cookie;
    for (int proto_id = proto_get_first_protocol(&proto_cookie); proto_id != -1;
         proto_id = proto_get_next_protocol(&proto_cookie)) {
        protocol_t *protocol = find_protocol_by_id(proto_id);
        if (!proto_is_protocol_enabled(protocol)) continue;

        QTreeWidgetItem *proto_ti = new QTreeWidgetItem(proto_type_);
        QString label = QString("%1 " UTF8_MIDDLE_DOT " %3")
                .arg(proto_get_protocol_short_name(protocol))
                .arg(proto_get_protocol_long_name(protocol));
        proto_ti->setText(0, label);
        proto_ti->setData(0, Qt::UserRole, QVariant::fromValue(proto_id));
        ptr_proto_list->append(proto_ti);
    }
    std::stable_sort(ptr_proto_list->begin(), ptr_proto_list->end(), compareTreeWidgetItems);

    foreach (QTreeWidgetItem *proto_ti, *ptr_proto_list) {
#ifdef DISPLAY_FILTER_EXPRESSION_DIALOG_USE_QPROMISE
        if (promise.isCanceled()) {
            delete proto_ti;
            continue;
        }
        promise.suspendIfRequested();
#endif
        void *field_cookie;
        int proto_id = proto_ti->data(0, Qt::UserRole).toInt();

        QList <QTreeWidgetItem *> field_list;
        for (header_field_info *hfinfo = proto_get_first_protocol_field(proto_id, &field_cookie); hfinfo != NULL;
             hfinfo = proto_get_next_protocol_field(proto_id, &field_cookie)) {
            if (hfinfo->same_name_prev_id != -1) continue; // Ignore duplicate names.

            QTreeWidgetItem *field_ti = new QTreeWidgetItem(field_type_);
            QString label = QString("%1 " UTF8_MIDDLE_DOT " %3").arg(hfinfo->abbrev).arg(hfinfo->name);
            field_ti->setText(0, label);
            field_ti->setData(0, Qt::UserRole, VariantPointer<header_field_info>::asQVariant(hfinfo));
            field_list << field_ti;
        }
        std::stable_sort(field_list.begin(), field_list.end(), compareTreeWidgetItems);
        proto_ti->addChildren(field_list);
#ifdef DISPLAY_FILTER_EXPRESSION_DIALOG_USE_QPROMISE
        if (!promise.addResult(proto_ti))
            delete proto_ti;
    }
#else
    }
    return ptr_proto_list;
#endif
}

DisplayFilterExpressionDialog::DisplayFilterExpressionDialog(QWidget *parent) :
    GeometryStateDialog(parent),
#ifdef DISPLAY_FILTER_EXPRESSION_DIALOG_USE_QPROMISE
    watcher(new QFutureWatcher<QTreeWidgetItem *>(nullptr)),
#else
    watcher(new QFutureWatcher<QList<QTreeWidgetItem *> *>(nullptr)),
#endif
    ui(new Ui::DisplayFilterExpressionDialog),
    ftype_(FT_NONE),
    field_(NULL)
{
    ui->setupUi(this);
    if (parent) loadGeometry(parent->width() * 2 / 3, parent->height());
    setAttribute(Qt::WA_DeleteOnClose, true);

    setWindowTitle(mainApp->windowTitleString(tr("Display Filter Expression")));
    setWindowIcon(mainApp->normalIcon());

    proto_initialize_all_prefixes();

    auto future = QtConcurrent::run(generateProtocolTreeItems);

    ui->fieldTreeWidget->setToolTip(ui->fieldLabel->toolTip());
    ui->searchLineEdit->setToolTip(ui->searchLabel->toolTip());
    ui->relationListWidget->setToolTip(ui->relationLabel->toolTip());
    ui->valueLineEdit->setToolTip(ui->valueLabel->toolTip());
    ui->enumListWidget->setToolTip(ui->enumLabel->toolTip());
    ui->rangeLineEdit->setToolTip(ui->rangeLabel->toolTip());

    // Relation list
    new QListWidgetItem("is present", ui->relationListWidget, present_op_);
    new QListWidgetItem("==", ui->relationListWidget, any_eq_op_);
    new QListWidgetItem("!=", ui->relationListWidget, all_ne_op_);
    new QListWidgetItem("===", ui->relationListWidget, all_eq_op_);
    new QListWidgetItem("!==", ui->relationListWidget, any_ne_op_);
    new QListWidgetItem(">", ui->relationListWidget, gt_op_);
    new QListWidgetItem("<", ui->relationListWidget, lt_op_);
    new QListWidgetItem(">=", ui->relationListWidget, ge_op_);
    new QListWidgetItem("<=", ui->relationListWidget, le_op_);
    new QListWidgetItem("contains", ui->relationListWidget, contains_op_);
    new QListWidgetItem("matches", ui->relationListWidget, matches_op_);
    new QListWidgetItem("in", ui->relationListWidget, in_op_);

    value_label_pfx_ = ui->valueLabel->text();

    connect(ui->anyRadioButton, &QAbstractButton::toggled, this, &DisplayFilterExpressionDialog::updateWidgets);
    connect(ui->allRadioButton, &QAbstractButton::toggled, this, &DisplayFilterExpressionDialog::updateWidgets);
    connect(ui->valueLineEdit, &QLineEdit::textEdited, this, &DisplayFilterExpressionDialog::updateWidgets);
    connect(ui->rangeLineEdit, &QLineEdit::textEdited, this, &DisplayFilterExpressionDialog::updateWidgets);

    updateWidgets();
    ui->searchLineEdit->setReadOnly(true);

#ifdef DISPLAY_FILTER_EXPRESSION_DIALOG_USE_QPROMISE
    connect(watcher, &QFutureWatcher<QTreeWidgetItem *>::resultReadyAt, this, &DisplayFilterExpressionDialog::addTreeItem);
    connect(watcher, &QFutureWatcher<QTreeWidgetItem *>::finished, this, &DisplayFilterExpressionDialog::fillTree);
#else
    connect(watcher, &QFutureWatcher<QList<QTreeWidgetItem *> *>::finished, this, &DisplayFilterExpressionDialog::fillTree);
    // If window is closed before future finishes, DisplayFilterExpressionDialog fillTree slot won't run
    // Register lambda to free up the list container and tree entries (if not consumed by fillTree())
    auto captured_watcher = this->watcher;
    connect(watcher, &QFutureWatcher<QList<QTreeWidgetItem *> *>::finished, [captured_watcher]() {
        QList<QTreeWidgetItem *> *items = captured_watcher->future().result();
        qDeleteAll(*items);
        delete items;
    });
#endif
    watcher->setFuture(future);
}

DisplayFilterExpressionDialog::~DisplayFilterExpressionDialog()
{
    if (watcher)
    {
#ifdef DISPLAY_FILTER_EXPRESSION_DIALOG_USE_QPROMISE
        watcher->future().cancel();
        qDeleteAll(watcher->future().results());
#endif
        watcher->waitForFinished();
        watcher->deleteLater();
    }
    delete ui;
}

#ifdef DISPLAY_FILTER_EXPRESSION_DIALOG_USE_QPROMISE
void DisplayFilterExpressionDialog::addTreeItem(int result)
{
    QTreeWidgetItem *item = watcher->future().resultAt(result);
    ui->fieldTreeWidget->invisibleRootItem()->addChild(item);
}
#endif

void DisplayFilterExpressionDialog::fillTree()
{
#ifndef DISPLAY_FILTER_EXPRESSION_DIALOG_USE_QPROMISE
    QList<QTreeWidgetItem *> *items = watcher->future().result();
    ui->fieldTreeWidget->invisibleRootItem()->addChildren(*items);
    // fieldTreeWidget now owns all items
    items->clear();
#endif
    watcher->deleteLater();
    watcher = nullptr;

    ui->searchLineEdit->setReadOnly(false);
}

void DisplayFilterExpressionDialog::updateWidgets()
{
    bool rel_enable = field_ != NULL;

    ui->relationLabel->setEnabled(rel_enable);
    ui->relationListWidget->setEnabled(rel_enable);
    ui->hintLabel->clear();

    bool quantity_enable = false;
    bool value_enable = false;
    bool enum_enable = false;
    bool enum_multi_enable = false;
    bool range_enable = false;

    QString filter;
    if (field_) {
        filter = field_;
        QListWidgetItem *rli = ui->relationListWidget->currentItem();
        if (rli && rli->type() > all_ne_op_) {
            quantity_enable = true;
            if (ui->anyRadioButton->isChecked()) {
                filter.prepend("any ");
            }
            else if (ui->allRadioButton->isChecked()) {
                filter.prepend("all ");
            }
            else {
                ws_assert_not_reached();
            }
        }
        if (rli && rli->type() != present_op_) {
            value_enable = true;
            if (ftype_can_slice(ftype_)) {
                range_enable = true;
            }
            enum_enable = ui->enumListWidget->count() > 0;
            filter.append(QString(" %1").arg(rli->text()));
        }
        if (value_enable && !ui->valueLineEdit->text().isEmpty()) {
            if (rli && rli->type() == in_op_) {
                filter.append(QString(" {%1}").arg(ui->valueLineEdit->text()));
                enum_multi_enable = enum_enable;
            } else {
                if (ftype_ == FT_STRING) {
                    filter.append(QString(" \"%1\"").arg(ui->valueLineEdit->text()));
                } else {
                    filter.append(QString(" %1").arg(ui->valueLineEdit->text()));
                }
            }
        }
    }

    ui->quantityLabel->setEnabled(quantity_enable);
    ui->allRadioButton->setEnabled(quantity_enable);
    ui->anyRadioButton->setEnabled(quantity_enable);

    ui->valueLabel->setEnabled(value_enable);
    ui->valueLineEdit->setEnabled(value_enable);

    ui->enumLabel->setEnabled(enum_enable);
    ui->enumListWidget->setEnabled(enum_enable);
    ui->enumListWidget->setSelectionMode(enum_multi_enable ?
        QAbstractItemView::ExtendedSelection : QAbstractItemView::SingleSelection);

    ui->rangeLabel->setEnabled(range_enable);
    ui->rangeLineEdit->setEnabled(range_enable);

    ui->displayFilterLineEdit->setText(filter);

    QString hint = "<small><i>";
    if (ui->fieldTreeWidget->selectedItems().count() < 1) {
        hint.append(tr("Select a field name to get started"));
    } else if (ui->displayFilterLineEdit->syntaxState() != SyntaxLineEdit::Valid) {
        hint.append(ui->displayFilterLineEdit->syntaxErrorMessage());
    } else {
        hint.append(tr("Click OK to insert this filter"));
    }
    hint.append("</i></small>");
    ui->hintLabel->setText(hint);

    QPushButton *ok_bt = ui->buttonBox->button(QDialogButtonBox::Ok);
    if (ok_bt) {
        bool ok_enable = !(ui->displayFilterLineEdit->text().isEmpty()
                || (ui->displayFilterLineEdit->syntaxState() == SyntaxLineEdit::Invalid));
        ok_bt->setEnabled(ok_enable);
    }
}

void DisplayFilterExpressionDialog::fillEnumBooleanValues(const true_false_string *tfs)
{
    QListWidgetItem *eli = new QListWidgetItem(tfs_get_string(true, tfs), ui->enumListWidget);
    eli->setData(Qt::UserRole, QString("1"));
    eli = new QListWidgetItem(tfs_get_string(false, tfs), ui->enumListWidget);
    eli->setData(Qt::UserRole, QString("0"));
}

void DisplayFilterExpressionDialog::fillEnumIntValues(const _value_string *vals, int base)
{
    if (!vals) return;

    for (int i = 0; vals[i].strptr != NULL; i++) {
        QListWidgetItem *eli = new QListWidgetItem(vals[i].strptr, ui->enumListWidget);
        eli->setData(Qt::UserRole, int_to_qstring(vals[i].value, 0, base));
    }
}

void DisplayFilterExpressionDialog::fillEnumInt64Values(const _val64_string *vals64, int base)
{
    if (!vals64) return;

    for (int i = 0; vals64[i].strptr != NULL; i++) {
        QListWidgetItem *eli = new QListWidgetItem(vals64[i].strptr, ui->enumListWidget);
        eli->setData(Qt::UserRole, int_to_qstring(vals64[i].value, 0, base));
    }
}

void DisplayFilterExpressionDialog::fillEnumRangeValues(const _range_string *rvals)
{
    if (!rvals) return;

    for (int i = 0; rvals[i].strptr != NULL; i++) {
        QString range_text = rvals[i].strptr;

        // Tell the user which values are valid here. Default to value_min below.
        if (rvals[i].value_min != rvals[i].value_max) {
            range_text.append(QString(" (%1 valid)").arg(range_to_qstring(&rvals[i])));
        }

        QListWidgetItem *eli = new QListWidgetItem(range_text, ui->enumListWidget);
        eli->setData(Qt::UserRole, QString::number(rvals[i].value_min));
    }
}

void DisplayFilterExpressionDialog::on_fieldTreeWidget_itemSelectionChanged()
{
    ftype_ = FT_NONE;
    field_ = NULL;
    QTreeWidgetItem *cur_fti = NULL;

    if (ui->fieldTreeWidget->selectedItems().count() > 0) {
        cur_fti = ui->fieldTreeWidget->selectedItems()[0];
    }
    ui->valueLineEdit->clear();
    ui->enumListWidget->clear();
    ui->rangeLineEdit->clear();

    if (cur_fti && cur_fti->type() == proto_type_) {
        ftype_ = FT_PROTOCOL;
        field_ = proto_get_protocol_filter_name(cur_fti->data(0, Qt::UserRole).toInt());
    } else if (cur_fti && cur_fti->type() == field_type_) {
        header_field_info *hfinfo = VariantPointer<header_field_info>::asPtr(cur_fti->data(0, Qt::UserRole));
        if (hfinfo) {
            ftype_ = hfinfo->type;
            field_ = hfinfo->abbrev;

            switch(ftype_) {
            case FT_BOOLEAN:
                // Let the user select the "True" and "False" values.
                fillEnumBooleanValues((const true_false_string *)hfinfo->strings);
                break;
            case FT_UINT8:
            case FT_UINT16:
            case FT_UINT24:
            case FT_UINT32:
            case FT_INT8:
            case FT_INT16:
            case FT_INT24:
            case FT_INT32:
            {
                int base;

                switch (hfinfo->display & FIELD_DISPLAY_E_MASK) {
                case BASE_HEX:
                case BASE_HEX_DEC:
                    base = 16;
                    break;
                case BASE_OCT:
                    base = 8;
                    break;
                default:
                    base = 10;
                    break;
                }
                // Let the user select from a list of value_string or range_string values.
                if (hfinfo->strings && ! ((hfinfo->display & FIELD_DISPLAY_E_MASK) == BASE_CUSTOM)) {
                    if (hfinfo->display & BASE_RANGE_STRING) {
                        fillEnumRangeValues((const range_string *)hfinfo->strings);
                    } else if (hfinfo->display & BASE_VAL64_STRING) {
                        const val64_string *vals = (const val64_string *)hfinfo->strings;
                        fillEnumInt64Values(vals, base);
                    } else { // Plain old value_string / VALS
                        const value_string *vals = (const value_string *)hfinfo->strings;
                        if (hfinfo->display & BASE_EXT_STRING)
                            vals = VALUE_STRING_EXT_VS_P((const value_string_ext *)vals);
                        fillEnumIntValues(vals, base);
                    }
                }
                break;
            }
            default:
                break;
            }
        }
    }
    if (ui->enumListWidget->count() > 0) {
        ui->enumListWidget->setCurrentRow(0);
    }

    bool all_show = field_ != NULL;
    for (int i = 0; i < ui->relationListWidget->count(); i++) {
        QListWidgetItem *li = ui->relationListWidget->item(i);
        switch (li->type()) {
        case any_eq_op_:
        case all_eq_op_:
        case any_ne_op_:
        case all_ne_op_:
            li->setHidden(!ftype_can_eq(ftype_) && !(ftype_can_slice(ftype_) && ftype_can_eq(FT_BYTES)));
            break;
        case gt_op_:
        case lt_op_:
        case ge_op_:
        case le_op_:
        case in_op_:
            li->setHidden(!ftype_can_cmp(ftype_) && !(ftype_can_slice(ftype_) && ftype_can_cmp(FT_BYTES)));
            break;
        case contains_op_:
            li->setHidden(!ftype_can_contains(ftype_) && !(ftype_can_slice(ftype_) && ftype_can_contains(FT_BYTES)));
            break;
        case matches_op_:
            li->setHidden(!ftype_can_matches(ftype_) && !(ftype_can_slice(ftype_) && ftype_can_matches(FT_BYTES)));
            break;
        default:
            li->setHidden(!all_show);
            break;
        }
    }
    if (all_show) {
        // Select "==" if it's present and we have a value, "is present" otherwise
        int row = ui->relationListWidget->count() > 1 && ui->enumListWidget->count() > 0 ? 1 : 0;
        ui->relationListWidget->setCurrentRow(row);
    }

    if (ftype_ != FT_NONE) {
        ui->valueLabel->setText(QString("%1 (%2)")
                                .arg(value_label_pfx_)
                                .arg(ftype_pretty_name(ftype_)));
    } else {
        ui->valueLabel->setText(value_label_pfx_);
    }

    updateWidgets();
}

void DisplayFilterExpressionDialog::on_relationListWidget_itemSelectionChanged()
{
    updateWidgets();
}

void DisplayFilterExpressionDialog::on_enumListWidget_itemSelectionChanged()
{
    QStringList values;
    QList<QListWidgetItem *> items = ui->enumListWidget->selectedItems();
    QList<QListWidgetItem *>::const_iterator it = items.constBegin();
    while (it != items.constEnd())
    {
        values << (*it)->data(Qt::UserRole).toString();
        ++it;
    }

    ui->valueLineEdit->setText(values.join(" "));

    updateWidgets();
}

void DisplayFilterExpressionDialog::on_searchLineEdit_textChanged(const QString &search_re)
{
    ui->fieldTreeWidget->setUpdatesEnabled(false);
    QTreeWidgetItemIterator it(ui->fieldTreeWidget);
    QRegularExpression regex(search_re, QRegularExpression::CaseInsensitiveOption);
    if (! regex.isValid())
        return;

    while (*it) {
        bool hidden = true;
        if (search_re.isEmpty() || (*it)->text(0).contains(regex)) {
            hidden = false;
            if ((*it)->type() == field_type_) {
                (*it)->parent()->setHidden(false);
            }
        }
        (*it)->setHidden(hidden);
        ++it;
    }
    ui->fieldTreeWidget->setUpdatesEnabled(true);
}

void DisplayFilterExpressionDialog::on_buttonBox_accepted()
{
    emit insertDisplayFilter(ui->displayFilterLineEdit->text());
}

void DisplayFilterExpressionDialog::on_buttonBox_helpRequested()
{
    mainApp->helpTopicAction(HELP_FILTER_EXPRESSION_DIALOG);
}
