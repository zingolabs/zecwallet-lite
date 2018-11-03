#include "mainwindow.h"
#include "ui_mainwindow.h"
#include "ui_privkey.h"
#include "ui_about.h"
#include "ui_settings.h"
#include "ui_turnstile.h"
#include "ui_turnstileprogress.h"
#include "rpc.h"
#include "balancestablemodel.h"
#include "settings.h"
#include "utils.h"
#include "turnstile.h"
#include "senttxstore.h"
#include "connection.h"

#include "precompiled.h"

using json = nlohmann::json;

MainWindow::MainWindow(QWidget *parent) :
    QMainWindow(parent),
    ui(new Ui::MainWindow)
{
    ui->setupUi(this);

    // Status Bar
    setupStatusBar();
    
    // Settings editor 
    setupSettingsModal();

    // Set up exit action
    QObject::connect(ui->actionExit, &QAction::triggered, this, &MainWindow::close);

    // Set up donate action
    QObject::connect(ui->actionDonate, &QAction::triggered, this, &MainWindow::donate);

    // Set up check for updates action
    QObject::connect(ui->actionCheck_for_Updates, &QAction::triggered, [=] () {
        QDesktopServices::openUrl(QUrl("https://github.com/adityapk00/zec-qt-wallet/releases"));
    });

    // Import Private Key
    QObject::connect(ui->actionImport_Private_Key, &QAction::triggered, this, &MainWindow::importPrivKey);

    // Export All Private Keys
    QObject::connect(ui->actionExport_All_Private_Keys, &QAction::triggered, this, &MainWindow::exportAllKeys);

    // Set up about action
    QObject::connect(ui->actionAbout, &QAction::triggered, [=] () {
        QDialog aboutDialog(this);
        Ui_about about;
        about.setupUi(&aboutDialog);

        QString version    = QString("Version ") % QString(APP_VERSION) % " (" % QString(__DATE__) % ")";
        about.versionLabel->setText(version);
        
        aboutDialog.exec();
    });

    // Initialize to the balances tab
    ui->tabWidget->setCurrentIndex(0);

    setupSendTab();
    setupTransactionsTab();
    setupRecieveTab();
    setupBalancesTab();
    setupTurnstileDialog();

    rpc = new RPC(this);

    restoreSavedStates();
}
 

void MainWindow::restoreSavedStates() {
    QSettings s;
    restoreGeometry(s.value("geometry").toByteArray());

    ui->balancesTable->horizontalHeader()->restoreState(s.value("baltablegeometry").toByteArray());
    ui->transactionsTable->horizontalHeader()->restoreState(s.value("tratablegeometry").toByteArray());
}

void MainWindow::closeEvent(QCloseEvent* event) {
    QSettings s;

    s.setValue("geometry", saveGeometry());
    s.setValue("baltablegeometry", ui->balancesTable->horizontalHeader()->saveState());
    s.setValue("tratablegeometry", ui->transactionsTable->horizontalHeader()->saveState());

    // Let the RPC know to shutdown any running service.
    rpc->closeEvent();

    // Bubble up
    QMainWindow::closeEvent(event);
}

void MainWindow::turnstileProgress() {
    Ui_TurnstileProgress progress;
    QDialog d(this);
    progress.setupUi(&d);

    QIcon icon = QApplication::style()->standardIcon(QStyle::SP_MessageBoxWarning);
    progress.msgIcon->setPixmap(icon.pixmap(64, 64));

    bool migrationFinished = false;
    auto fnUpdateProgressUI = [=, &migrationFinished] () mutable {
        // Get the plan progress
        if (rpc->getTurnstile()->isMigrationPresent()) {
            auto curProgress = rpc->getTurnstile()->getPlanProgress();
            
            progress.progressTxt->setText(QString::number(curProgress.step) % QString(" / ") % QString::number(curProgress.totalSteps));
            progress.progressBar->setValue(100 * curProgress.step / curProgress.totalSteps);
            
            auto nextTxBlock = curProgress.nextBlock - Settings::getInstance()->getBlockNumber();
            
            progress.fromAddr->setText(curProgress.from);
            progress.toAddr->setText(curProgress.to);

            if (curProgress.step == curProgress.totalSteps) {
                migrationFinished = true;
                auto txt = QString("Turnstile migration finished");
                if (curProgress.hasErrors) {
                    txt = txt + ". There were some errors.\n\nYour funds are all in your wallet, so you should be able to finish moving them manually.";
                }
                progress.nextTx->setText(txt);
            } else {
                progress.nextTx->setText(QString("Next transaction in ") 
                                    % QString::number(nextTxBlock < 0 ? 0 : nextTxBlock)
                                    % " blocks via " % curProgress.via % "\n" 
                                    % (nextTxBlock <= 0 ? "(waiting for confirmations)" : ""));
            }
            
        } else {
            progress.progressTxt->setText("");
            progress.progressBar->setValue(0);
            progress.nextTx->setText("No turnstile migration is in progress");
        }
    };

    QTimer progressTimer(this);        
    QObject::connect(&progressTimer, &QTimer::timeout, fnUpdateProgressUI);
    progressTimer.start(Utils::updateSpeed);
    fnUpdateProgressUI();
    
    auto curProgress = rpc->getTurnstile()->getPlanProgress();

    // Abort button
    if (curProgress.step != curProgress.totalSteps)
        progress.buttonBox->button(QDialogButtonBox::Discard)->setText("Abort");
    else
        progress.buttonBox->button(QDialogButtonBox::Discard)->setVisible(false);

    QObject::connect(progress.buttonBox->button(QDialogButtonBox::Discard), &QPushButton::clicked, [&] () {
        if (curProgress.step != curProgress.totalSteps) {
            auto abort = QMessageBox::warning(this, "Are you sure you want to Abort?",
                                    "Are you sure you want to abort the migration?\nAll further transactions will be cancelled.\nAll your funds are still in your wallet.",
                                    QMessageBox::Yes, QMessageBox::No);
            if (abort == QMessageBox::Yes) {
                rpc->getTurnstile()->removeFile();
                d.close();
                ui->statusBar->showMessage("Automatic Sapling turnstile migration aborted.");
            }
        }
    });

    d.exec();    
    if (migrationFinished || curProgress.step == curProgress.totalSteps) {
        // Finished, so delete the file
        rpc->getTurnstile()->removeFile();
    }    
}

void MainWindow::turnstileDoMigration(QString fromAddr) {
    // Return if there is no connection
    if (rpc->getAllZAddresses() == nullptr)
        return;

    Ui_Turnstile turnstile;
    QDialog d(this);
    turnstile.setupUi(&d);

    QIcon icon = QApplication::style()->standardIcon(QStyle::SP_MessageBoxInformation);
    turnstile.msgIcon->setPixmap(icon.pixmap(64, 64));

    auto fnGetAllSproutBalance = [=] () {
        double bal = 0;
        for (auto addr : *rpc->getAllZAddresses()) {
            if (Settings::getInstance()->isSproutAddress(addr)) {
                bal += rpc->getAllBalances()->value(addr);
            }
        }

        return bal;
    };

    //turnstile.migrateZaddList->addItem("All Sprout z-Addrs");
    turnstile.fromBalance->setText(Settings::getInstance()->getZECUSDDisplayFormat(fnGetAllSproutBalance()));
    for (auto addr : *rpc->getAllZAddresses()) {
        if (Settings::getInstance()->isSaplingAddress(addr)) {
            turnstile.migrateTo->addItem(addr);
        } else {
            turnstile.migrateZaddList->addItem(addr);
        }
    }

    auto fnUpdateSproutBalance = [=] (QString addr) {
        double bal = 0;
        if (addr.startsWith("All")) {
            bal = fnGetAllSproutBalance();
        } else {
            bal = rpc->getAllBalances()->value(addr);
        }

        auto balTxt = Settings::getInstance()->getZECUSDDisplayFormat(bal);
        
        if (bal < Turnstile::minMigrationAmount) {
            turnstile.fromBalance->setStyleSheet("color: red;");
            turnstile.fromBalance->setText(balTxt % " [You need at least " 
                        % Settings::getInstance()->getZECDisplayFormat(Turnstile::minMigrationAmount)
                        % " for automatic migration]");
            turnstile.buttonBox->button(QDialogButtonBox::Ok)->setEnabled(false);
        } else {
            turnstile.fromBalance->setStyleSheet("");
            turnstile.buttonBox->button(QDialogButtonBox::Ok)->setEnabled(true);
            turnstile.fromBalance->setText(balTxt);
        }
    };

    if (!fromAddr.isEmpty())
        turnstile.migrateZaddList->setCurrentText(fromAddr);

    fnUpdateSproutBalance(turnstile.migrateZaddList->currentText());
    

    // Combo box selection event
    QObject::connect(turnstile.migrateZaddList, &QComboBox::currentTextChanged, fnUpdateSproutBalance);
        
    // Privacy level combobox
    // Num tx over num blocks
    QList<std::tuple<QString, int, int>> privOptions; 
    privOptions.push_back(std::make_tuple<QString, int, int>("Good", 3, 576));
    privOptions.push_back(std::make_tuple<QString, int, int>("Excellent", 5, 1152));
    privOptions.push_back(std::make_tuple<QString, int, int>("Paranoid", 10, 2304));

    QObject::connect(turnstile.privLevel, QOverload<int>::of(&QComboBox::currentIndexChanged), [=] (auto idx) {
        // Update the fees
        turnstile.minerFee->setText(
            Settings::getInstance()->getZECUSDDisplayFormat(std::get<1>(privOptions[idx]) * Utils::getMinerFee()));
    });

    for (auto i : privOptions) {
        turnstile.privLevel->addItem(std::get<0>(i) % " - " 
                % QString::number(std::get<1>(i)) % " tx over " 
                % QString::number(std::get<2>(i)) % " blocks ("
                % QString::number((int)(std::get<2>(i) / 24 / 24)) % " days)" // 24 blks/hr * 24 hrs per day
        );
    }
    
    turnstile.buttonBox->button(QDialogButtonBox::Ok)->setText("Start");

    if (d.exec() == QDialog::Accepted) {
        auto privLevel = privOptions[turnstile.privLevel->currentIndex()];
        rpc->getTurnstile()->planMigration(
            turnstile.migrateZaddList->currentText(), 
            turnstile.migrateTo->currentText(),
            std::get<1>(privLevel), std::get<2>(privLevel));

        QMessageBox::information(this, "Backup your wallet.dat", 
                                    "The migration will now start. You can check progress in the File -> Sapling Turnstile menu.\n\nYOU MUST BACKUP YOUR wallet.dat NOW!\n\nNew Addresses have been added to your wallet which will be used for the migration.", 
                                    QMessageBox::Ok);
    }
}

void MainWindow::setupTurnstileDialog() {        
    // Turnstile migration
    QObject::connect(ui->actionTurnstile_Migration, &QAction::triggered, [=] () {
        // If there is current migration that is present, show the progress button
        if (rpc->getTurnstile()->isMigrationPresent())
            turnstileProgress();
        else    
            turnstileDoMigration();        
    });

}

void MainWindow::setupStatusBar() {
    // Status Bar
    loadingLabel = new QLabel();
    loadingMovie = new QMovie(":/icons/res/loading.gif");
    loadingMovie->setScaledSize(QSize(32, 16));
    loadingMovie->start();
    loadingLabel->setAttribute(Qt::WA_NoSystemBackground);
    loadingLabel->setMovie(loadingMovie);

    ui->statusBar->addPermanentWidget(loadingLabel);
    loadingLabel->setVisible(false);

    // Custom status bar menu
    ui->statusBar->setContextMenuPolicy(Qt::CustomContextMenu);
    QObject::connect(ui->statusBar, &QStatusBar::customContextMenuRequested, [=](QPoint pos) {
        auto msg = ui->statusBar->currentMessage();
        QMenu menu(this);

        if (!msg.isEmpty() && msg.startsWith(Utils::txidStatusMessage)) {
            auto txid = msg.split(":")[1].trimmed();
            menu.addAction("Copy txid", [=]() {
                QGuiApplication::clipboard()->setText(txid);
            });
            menu.addAction("View tx on block explorer", [=]() {
                QString url;
                if (Settings::getInstance()->isTestnet()) {
                    url = "https://explorer.testnet.z.cash/tx/" + txid;
                }
                else {
                    url = "https://explorer.zcha.in/transactions/" + txid;
                }
                QDesktopServices::openUrl(QUrl(url));
            });
        }

        menu.addAction("Refresh", [=]() {
            rpc->refresh(true);
        });
        QPoint gpos(mapToGlobal(pos).x(), mapToGlobal(pos).y() + this->height() - ui->statusBar->height());
        menu.exec(gpos);
    });

    statusLabel = new QLabel();
    ui->statusBar->addPermanentWidget(statusLabel);

    statusIcon = new QLabel();
    ui->statusBar->addPermanentWidget(statusIcon);
}

void MainWindow::setupSettingsModal() {    
    // Set up File -> Settings action
    QObject::connect(ui->actionSettings, &QAction::triggered, [=]() {
        QDialog settingsDialog(this);
        Ui_Settings settings;
        settings.setupUi(&settingsDialog);

        // Setup save sent check box
        QObject::connect(settings.chkSaveTxs, &QCheckBox::stateChanged, [=](auto checked) {
            Settings::getInstance()->setSaveZtxs(checked);
        });

        // Setup clear button
        QObject::connect(settings.btnClearSaved, &QCheckBox::clicked, [=]() {
            if (QMessageBox::warning(this, "Clear saved history?",
                "Shielded z-Address transactions are stored locally in your wallet, outside zcashd. You may delete this saved information safely any time for your privacy.\nDo you want to delete the saved shielded transactions now ?",
                QMessageBox::Yes, QMessageBox::Cancel)) {
                    SentTxStore::deleteHistory();
                    // Reload after the clear button so existing txs disappear
                    rpc->refresh(true);
            }
        });

        // Save sent transactions
        settings.chkSaveTxs->setChecked(Settings::getInstance()->getSaveZtxs());

        // Connection Settings
        QIntValidator validator(0, 65535);
        settings.port->setValidator(&validator);

        // Load current values into the dialog        
        auto conf = Settings::getInstance()->getSettings();
        settings.hostname->setText(conf.host);
        settings.port->setText(conf.port);
        settings.rpcuser->setText(conf.rpcuser);
        settings.rpcpassword->setText(conf.rpcpassword);

        // If values are coming from zcash.conf, then disable all the fields
        auto zcashConfLocation = Settings::getInstance()->getZcashdConfLocation();
        if (!zcashConfLocation.isEmpty()) {
            settings.confMsg->setText("Settings are being read from \n" + zcashConfLocation);
            settings.hostname->setEnabled(false);
            settings.port->setEnabled(false);
            settings.rpcuser->setEnabled(false);
            settings.rpcpassword->setEnabled(false);
        }
        else {
            settings.confMsg->setText("No local zcash.conf found. Please configure connection manually.");
            settings.hostname->setEnabled(true);
            settings.port->setEnabled(true);
            settings.rpcuser->setEnabled(true);
            settings.rpcpassword->setEnabled(true);
        }

        // Connection tab by default
        settings.tabWidget->setCurrentIndex(0);

        if (settingsDialog.exec() == QDialog::Accepted) {
            if (zcashConfLocation.isEmpty()) {
                // Save settings
                Settings::getInstance()->saveSettings(
                    settings.hostname->text(),
                    settings.port->text(),
                    settings.rpcuser->text(),
                    settings.rpcpassword->text());
                
                auto cl = new ConnectionLoader(this, rpc);
                cl->loadConnection();
            }
        };
    });

}

void MainWindow::donate() {
    // Set up a donation to me :)
    ui->Address1->setText(Utils::getDonationAddr(
                                Settings::getInstance()->isSaplingAddress(ui->inputsCombo->currentText())));
    ui->Address1->setCursorPosition(0);
    ui->Amount1->setText("0.01");
    ui->MemoTxt1->setText("Thanks for supporting zec-qt-wallet!");

    ui->statusBar->showMessage("Donate 0.01 " % Utils::getTokenName() % " to support zec-qt-wallet");

    // And switch to the send tab.
    ui->tabWidget->setCurrentIndex(1);
}

void MainWindow::doImport(QList<QString>* keys) {
    qDebug() << keys->size();
    if (keys->isEmpty()) {
        delete keys;
        ui->statusBar->showMessage("Private key import rescan finished");
        return;
    }

    // Pop the first key
    QString key = keys->first();
    keys->pop_front();
    bool rescan = keys->isEmpty();

    if (key.startsWith("S") ||
        key.startsWith("secret")) { // Z key
        rpc->importZPrivKey(key, rescan, [=] (auto) { this->doImport(keys); });                   
    } else {
        rpc->importTPrivKey(key, rescan, [=] (auto) { this->doImport(keys); });
    }
}


void MainWindow::importPrivKey() {
    QDialog d(this);
    Ui_PrivKey pui;
    pui.setupUi(&d);

    pui.buttonBox->button(QDialogButtonBox::Save)->setVisible(false);
    pui.helpLbl->setText(QString() %
                        "Please paste your private keys (z-Addr or t-Addr) here, one per line.\n" %
                        "The keys will be imported into your connected zcashd node");  

    if (d.exec() == QDialog::Accepted && !pui.privKeyTxt->toPlainText().trimmed().isEmpty()) {
        auto rawkeys = pui.privKeyTxt->toPlainText().trimmed().split("\n");

        QList<QString> keysTmp;
        // Filter out all the empty keys.
        std::copy_if(rawkeys.begin(), rawkeys.end(), std::back_inserter(keysTmp), [=] (auto key) {
            return !key.startsWith("#") && !key.trimmed().isEmpty();
        });

        auto keys = new QList<QString>();
        std::transform(keysTmp.begin(), keysTmp.end(), std::back_inserter(*keys), [=](auto key) {
            return key.trimmed().split(" ")[0];
        });

        // Start the import. The function takes ownership of keys
        doImport(keys);
        QMessageBox::information(this, 
            "Imported", "The keys were imported. It may take several minutes to rescan the blockchain. Until then, functionality may be limited",
            QMessageBox::Ok);
    }
}

void MainWindow::exportAllKeys() {
    QDialog d(this);
    Ui_PrivKey pui;
    pui.setupUi(&d);

    auto ps = this->geometry();
    QMargins margin = QMargins() + 50;
    d.setGeometry(ps.marginsRemoved(margin));

    pui.privKeyTxt->setPlainText("Loading...");
    pui.privKeyTxt->setReadOnly(true);
    pui.privKeyTxt->setLineWrapMode(QPlainTextEdit::LineWrapMode::NoWrap);
    pui.helpLbl->setText("These are all the private keys for all the addresses in your wallet");

    // Disable the save button until it finishes loading
    pui.buttonBox->button(QDialogButtonBox::Save)->setEnabled(false);
    pui.buttonBox->button(QDialogButtonBox::Ok)->setVisible(false);

    // Wire up save button
    QObject::connect(pui.buttonBox->button(QDialogButtonBox::Save), &QPushButton::clicked, [=] () {
        QString fileName = QFileDialog::getSaveFileName(this, tr("Save File"),
                           "zcash-all-privatekeys.txt");
        QFile file(fileName);
        if (!file.open(QIODevice::WriteOnly)) {
            QMessageBox::information(this, tr("Unable to open file"), file.errorString());
            return;
        }        
        QTextStream out(&file);
        out << pui.privKeyTxt->toPlainText();
    });

    // Call the API
    auto isDialogAlive = std::make_shared<bool>(true);
    rpc->getAllPrivKeys([=] (auto privKeys) {
        // Check to see if we are still showing.
        if (! *isDialogAlive.get()) return;

        QString allKeysTxt;
        for (auto keypair : privKeys) {
            allKeysTxt = allKeysTxt % keypair.second % " # addr=" % keypair.first % "\n";
        }

        pui.privKeyTxt->setPlainText(allKeysTxt);
        pui.buttonBox->button(QDialogButtonBox::Save)->setEnabled(true);
    });

    d.exec();
    *isDialogAlive.get() = false;
}

void MainWindow::setupBalancesTab() {
    ui->unconfirmedWarning->setVisible(false);

    // Setup context menu on balances tab
    ui->balancesTable->setContextMenuPolicy(Qt::CustomContextMenu);
    QObject::connect(ui->balancesTable, &QTableView::customContextMenuRequested, [=] (QPoint pos) {
        QModelIndex index = ui->balancesTable->indexAt(pos);
        if (index.row() < 0) return;

        index = index.sibling(index.row(), 0);
        auto addr = ui->balancesTable->model()->data(index).toString();

        QMenu menu(this);

        menu.addAction("Copy address", [=] () {
            QClipboard *clipboard = QGuiApplication::clipboard();
            clipboard->setText(addr);            
            ui->statusBar->showMessage("Copied to clipboard", 3 * 1000);
        });

        menu.addAction("Get private key", [=] () {
            auto fnCB = [=] (const json& reply) {
                auto privKey = QString::fromStdString(reply.get<json::string_t>());
                QDialog d(this);
                Ui_PrivKey pui;                
                pui.setupUi(&d);

                pui.helpLbl->setText("Private Key:");
                pui.privKeyTxt->setPlainText(privKey);
                pui.privKeyTxt->setReadOnly(true);
                pui.privKeyTxt->selectAll();
                pui.buttonBox->button(QDialogButtonBox::Save)->setVisible(false);
                pui.buttonBox->button(QDialogButtonBox::Ok)->setVisible(false);

                d.exec();
            };

            if (Settings::getInstance()->isZAddress(addr)) 
                rpc->getZPrivKey(addr, fnCB);
            else
                rpc->getTPrivKey(addr, fnCB);
        });

        menu.addAction("Send from " % addr.left(40) % (addr.size() > 40 ? "..." : ""), [=]() {
            // Find the inputs combo
            for (int i = 0; i < ui->inputsCombo->count(); i++) {
                if (ui->inputsCombo->itemText(i).startsWith(addr)) {
                    ui->inputsCombo->setCurrentIndex(i);
                    break;
                }
            }
            
            // And switch to the send tab.
            ui->tabWidget->setCurrentIndex(1);
        });

        if (addr.startsWith("t")) {
            menu.addAction("View on block explorer", [=] () {
                QString url;
                if (Settings::getInstance()->isTestnet()) {
                    url = "https://explorer.testnet.z.cash/address/" + addr;
                } else {
                    url = "https://explorer.zcha.in/accounts/" + addr;
                }
                QDesktopServices::openUrl(QUrl(url));
            });
        }

        if (Settings::getInstance()->isSproutAddress(addr)) {
            menu.addAction("Migrate to Sapling", [=] () {
                this->turnstileDoMigration(addr);
            });
        }

        menu.exec(ui->balancesTable->viewport()->mapToGlobal(pos));            
    });
}

void MainWindow::setupTransactionsTab() {
    // Set up context menu on transactions tab
    ui->transactionsTable->setContextMenuPolicy(Qt::CustomContextMenu);

    QObject::connect(ui->transactionsTable, &QTableView::customContextMenuRequested, [=] (QPoint pos) {
        QModelIndex index = ui->transactionsTable->indexAt(pos);
        if (index.row() < 0) return;

        QMenu menu(this);

        auto txModel = dynamic_cast<TxTableModel *>(ui->transactionsTable->model());

        QString txid = txModel->getTxId(index.row());
        QString memo = txModel->getMemo(index.row());

        menu.addAction("Copy txid", [=] () {            
            QGuiApplication::clipboard()->setText(txid);
            ui->statusBar->showMessage("Copied to clipboard", 3 * 1000);
        });
        menu.addAction("View on block explorer", [=] () {
            QString url;
            if (Settings::getInstance()->isTestnet()) {
                url = "https://explorer.testnet.z.cash/tx/" + txid;
            } else {
                url = "https://explorer.zcha.in/transactions/" + txid;
            }
            QDesktopServices::openUrl(QUrl(url));
        });
        if (!memo.isEmpty()) {
            menu.addAction("View Memo", [=] () {
                QMessageBox::information(this, "Memo", memo, QMessageBox::Ok);
            });
        }

        menu.exec(ui->transactionsTable->viewport()->mapToGlobal(pos));        
    });
}

void MainWindow::addNewZaddr(bool sapling) {
    rpc->newZaddr(sapling, [=] (json reply) {
        QString addr = QString::fromStdString(reply.get<json::string_t>());
        // Make sure the RPC class reloads the Z-addrs for future use
        rpc->refreshAddresses();

        // Just double make sure the Z-address is still checked
        if (( sapling && ui->rdioZSAddr->isChecked()) ||
            (!sapling && ui->rdioZAddr->isChecked())) {
            ui->listRecieveAddresses->insertItem(0, addr);
            ui->listRecieveAddresses->setCurrentIndex(0);

            ui->statusBar->showMessage(QString::fromStdString("Created new zAddr") %
                                       (sapling ? "(Sapling)" : "(Sprout)"), 
                                       10 * 1000);
        }
    });
}


// Adds sapling or sprout z-addresses to the combo box. Technically, returns a
// lambda, which can be connected to the appropriate signal
std::function<void(bool)> MainWindow::addZAddrsToComboList(bool sapling) {
    return [=] (bool checked) { 
        if (checked && this->rpc->getAllZAddresses() != nullptr) { 
            auto addrs = this->rpc->getAllZAddresses();
            ui->listRecieveAddresses->clear();

            std::for_each(addrs->begin(), addrs->end(), [=] (auto addr) {
                if ( (sapling &&  Settings::getInstance()->isSaplingAddress(addr)) ||
                    (!sapling && !Settings::getInstance()->isSaplingAddress(addr)))
                    ui->listRecieveAddresses->addItem(addr);
            }); 

            // If z-addrs are empty, then create a new one.
            if (addrs->isEmpty()) {
                addNewZaddr(sapling);
            }
        } 
    };
}

void MainWindow::setupRecieveTab() {
    auto addNewTAddr = [=] () {
        rpc->newTaddr([=] (json reply) {
                QString addr = QString::fromStdString(reply.get<json::string_t>());

                // Just double make sure the T-address is still checked
                if (ui->rdioTAddr->isChecked()) {
                    ui->listRecieveAddresses->insertItem(0, addr);
                    ui->listRecieveAddresses->setCurrentIndex(0);

                    ui->statusBar->showMessage("Created new t-Addr", 10 * 1000);
                }
            });
    };

    // Connect t-addr radio button
    QObject::connect(ui->rdioTAddr, &QRadioButton::toggled, [=] (bool checked) { 
        // Whenever the T-address is selected, we generate a new address, because we don't
        // want to reuse T-addrs
        if (checked && this->rpc->getUTXOs() != nullptr) { 
            auto utxos = this->rpc->getUTXOs();
            ui->listRecieveAddresses->clear();

            std::for_each(utxos->begin(), utxos->end(), [=] (auto& utxo) {
                auto addr = utxo.address;
                if (addr.startsWith("t") && ui->listRecieveAddresses->findText(addr) < 0) {
                    ui->listRecieveAddresses->addItem(addr);
                }
            });

            addNewTAddr();
        } 
    });


    // zAddr toggle button, one for sprout and one for sapling
    QObject::connect(ui->rdioZAddr,  &QRadioButton::toggled, addZAddrsToComboList(false));
    QObject::connect(ui->rdioZSAddr, &QRadioButton::toggled, addZAddrsToComboList(true));

    // Explicitly get new address button.
    QObject::connect(ui->btnRecieveNewAddr, &QPushButton::clicked, [=] () {
        if (ui->rdioZAddr->isChecked()) {
            addNewZaddr(false); 
        } else if (ui->rdioZSAddr->isChecked()) {
            addNewZaddr(true);
        } else if (ui->rdioTAddr->isChecked()) {
            addNewTAddr();
        }
    });

    // Focus enter for the Recieve Tab
    QObject::connect(ui->tabWidget, &QTabWidget::currentChanged, [=] (int tab) {
        if (tab == 2) {
            // Switched to recieve tab, so update everything. 

            // Hide Sapling radio button if sapling is not active
            if (Settings::getInstance()->isSaplingActive()) {
                ui->rdioZSAddr->setVisible(true);    
                ui->rdioZSAddr->setChecked(true);
                ui->rdioZAddr->setText("z-Addr(Sprout)");
            } else {
                ui->rdioZSAddr->setVisible(false);    
                ui->rdioZAddr->setChecked(true);
                ui->rdioZAddr->setText("z-Addr");   // Don't use the "Sprout" label if there's no sapling
            }
            
            // And then select the first one
            ui->listRecieveAddresses->setCurrentIndex(0);
        }
    });

    // Select item in address list
    QObject::connect(ui->listRecieveAddresses, 
        QOverload<const QString &>::of(&QComboBox::currentIndexChanged), [=] (const QString& addr) {
        if (addr.isEmpty()) {
            // Draw empty stuff

            ui->txtRecieve->clear();
            ui->qrcodeDisplay->clear();
            return;
        }

        ui->txtRecieve->setPlainText(addr);       
        ui->qrcodeDisplay->setAddress(addr);
    });    

}

MainWindow::~MainWindow()
{
    delete ui;
    delete rpc;

    delete loadingMovie;
}
