/* Copyright (C) 2002-2005 RealVNC Ltd.  All Rights Reserved.
 * Copyright (C) 2011 Brian P. Hinz
 *
 * This is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This software is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this software; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301,
 * USA.
 */

package com.tigervnc.vncviewer;

import java.awt.*;
import java.awt.event.*;
import java.awt.datatransfer.Clipboard;
import java.awt.datatransfer.StringSelection;
import javax.swing.*;
import javax.swing.border.*;
import com.tigervnc.rfb.LogWriter;

class ClipboardDialog extends Dialog implements ActionListener {

  public ClipboardDialog(CConn cc_) {
    super(false);
    cc = cc_;
    setTitle("VNC clipboard");
    JPanel pt = new JPanel();
    textArea = new JTextArea(5,50);
    textArea.setBorder(BorderFactory.createLineBorder(Color.gray));
    textArea.setLineWrap(true);
    textArea.setWrapStyleWord(true);
    JScrollPane sp = new JScrollPane(textArea);
    pt.add(sp, BorderLayout.CENTER);
    getContentPane().add("North", pt);

    JPanel pb = new JPanel();
    clearButton = new JButton("Clear");
    pb.add(clearButton);
    clearButton.addActionListener(this);
    sendButton = new JButton("Send to VNC server");
    pb.add(sendButton);
    sendButton.addActionListener(this);
    cancelButton = new JButton("Cancel");
    pb.add(cancelButton);
    cancelButton.addActionListener(this);
    getContentPane().add("South", pb);

    pack();
  }

  public boolean compareContentsTo(String str) {
    return str.equals(textArea.getText());

  }

  public void setContents(String str) {
    textArea.setText(str);
  }

  public String getContents() {
    return textArea.getText();
  }

  public void serverCutText(String str, int len) {
    setContents(str);
    SecurityManager sm = System.getSecurityManager();
    try {
      if (sm != null) sm.checkSystemClipboardAccess();
      Clipboard cb = Toolkit.getDefaultToolkit().getSystemClipboard();
      if (cb != null) {
        StringSelection ss = new StringSelection(str);
        try {
          cb.setContents(ss, null);
        } catch(Exception e) {
          vlog.debug(e.getMessage());
        }
      }
    } catch(SecurityException e) {
      vlog.debug("Cannot access the system clipboard: "+e.getMessage());
    }
  }

  public void setSendingEnabled(boolean b) {
    sendButton.setEnabled(b);
  }

  public void actionPerformed(ActionEvent e) {
    Object s = e.getSource();
    if (s instanceof JButton && (JButton)s == clearButton) {
      serverCutText(new String(""), 0);
    } else if (s instanceof JButton && (JButton)s == sendButton) {
      cc.writeClientCutText(textArea.getText(), textArea.getText().length());
      endDialog();
    } else if (s instanceof JButton && (JButton)s == cancelButton) {
      endDialog();
    }
  }

  CConn cc;
  JTextArea textArea;
  JButton clearButton, sendButton, cancelButton;
  static LogWriter vlog = new LogWriter("ClipboardDialog");
}
