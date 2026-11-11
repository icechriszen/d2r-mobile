"""游戏内云存档子面板；所有图元取自原版选项、前端按钮和手柄选择框。
In-game cloud-save subpanel; all art comes from original options, front-end buttons and controller selectors.
"""

def panel():
    def text(name, x, y, w, h, key='', size='$MediumFontSize'):
        return {'type':'TextBoxWidget','name':name,'fields':{'rect':{'x':x,'y':y,'width':w,'height':h},'text':key,
            'style':'$StyleSettingsText','style/pointSize':size,'style/options':{'newlineHandling':'standard','lineWrap':True},'style/alignment':{'h':'left','v':'top'}}}
    rows=[text('TouchCS_Left',0,0,790,800),text('TouchCS_Right',850,0,790,800)]
    for i in range(14):
        rows.append({'type':'Widget','name':f'TouchCS_Row{i}','fields':{'rect':{'x':0,'y':900+i*150,'width':1640,'height':140}},'children':[
            {'type':'ButtonWidget','name':f'TouchCS_Action{i}','fields':{'rect':{'x':0,'y':0,'scale':1.2},'filename':'FrontEnd/HD/Final/FrontEnd_ButtonLarge',
                'pressedFrame':1,'disabledFrame':2,'hoveredFrame':3,'sound':'cursor_menu_select_hd','onClickMessage':f'PanelManager:ClosePanel:TouchCS_Action{i}'},
             'children':[text(f'TouchCS_Label{i}',24,12,550,120,size=30),text(f'TouchCS_SubLabel{i}',24,76,550,48,size=30)]}]})
    for row in rows:
        if row.get('name','').startswith('TouchCS_Row'):
            for label in row['children'][0]['children']:
                label['fields']['style/alignment']={'h':'center','v':'center'}
                label['fields']['style/options']['lineWrap']=False
    rows.append({'type':'NineTileImageWidget','name':'TouchCS_Selection','fields':{'rect':{'x':99999,'y':0,'width':1660,'height':140},'backgroundColor':[0,0,0,0],
        'filename':'Controller/PANEL/ControllerSelector/ControllerSelectorBorder','tileScale':0.5}})
    return {'type':'Widget','name':'TouchCS_Modal','fields':{'priority':20000,'rect':{'x':99999,'y':-270,'width':1850,'height':1460}},'children':[
        {'type':'RectangleWidget','name':'TouchCS_Dim','fields':{'fitToScreen':True,'color':[0,0,0,1]},'children':[{'type':'ClickCatcherWidget','name':'TouchCS_Catcher','fields':{'fitToParent':True}}]},
        {'type':'ImageWidget','name':'TouchCS_Background','fields':{'rect':{'x':-55,'y':0},'filename':'PANEL/Options/FrontEndOptionsBG'}},
        text('TouchCS_Title',40,65,1550,90,'@dmSyncTitle','$LargeFontSize'),text('TouchCS_Status',40,165,1640,125),
        {'type':'ButtonLegendWidget','name':'TouchCS_Legend','fields':{'rect':{'x':40,'y':1340,'width':1640,'height':100},'text':{'style':'$StyleButtonLegend'}}},
        text('TouchCS_KeyboardHint',40,1350,1640,90,'@dmSyncKeyboardHint',30),
        {'type':'ButtonWidget','name':'TouchCS_Back','fields':{'rect':{'x':1710,'y':20},'filename':'PANEL/closebtn_4x','hoveredFrame':3,'sound':'cursor_close_window_hd',
            'onClickMessage':'PanelManager:ClosePanel:TouchCS_Back'}},
        {'type':'Widget','name':'TouchCS_ListBox','fields':{'rect':{'x':40,'y':310,'width':1640,'height':1000}},'children':[
            {'type':'ScrollViewWidget','name':'TouchCS_Scroll','fields':{'fitToParent':True,'clipChildrenDraw':True,'scrollControllerName':'TouchCS_ScrollController'},'children':[
                {'type':'Widget','name':'TouchCS_Rows','fields':{'rect':{'x':0,'y':0,'width':1640,'height':3000}},'children':rows}]}]},
        {'type':'ScrollControllerWidget','name':'TouchCS_ScrollController','fields':{'rect':{'x':1730,'y':310,'width':26,'height':1000},'viewName':'TouchCS_Scroll',
            'upArrowFilepath':'FrontEnd/HD/Final/FrontEnd_ScrollUpBtn','downArrowFilepath':'FrontEnd/HD/Final/FrontEnd_ScrollDownBtn','barFilepath':'PauseMenu/VerticalIndicator',
            'wheelScrollSound':'cursor_scroll_hd','buttonScrollSound':'cursor_scroll_hd'}}]}


def add_notice(source):
    """无按钮提示条：复用原版提示边框，不能接收输入或抢焦点。
    Button-free notice: reuse the original tooltip border without receiving input or focus.
    """
    import json
    import re
    notice={'type':'Widget','name':'TouchCS_Notice','fields':{'priority':19000,'anchor':{'x':0.5},'rect':{'x':99999,'y':200,'width':1500,'height':180}},'children':[
        {'type':'NineTileImageWidget','name':'TouchCS_NoticeBorder','fields':{'fitToParent':True,'filename':'PANEL/TooltipBorder/TooltipBorder','backgroundColor':[0,0,0,0.8],'tileScale':1}},
        {'type':'TextBoxWidget','name':'TouchCS_NoticeText','fields':{'rect':{'x':30,'y':20,'width':1440,'height':140},'text':'','style':'$StyleSettingsText','style/options':{'lineWrap':True,'newlineHandling':'standard'}}}]}
    match=re.search(r'"children"\s*:\s*\[',source)
    if not match: raise ValueError('cloud notice host has no children')
    return source[:match.end()]+json.dumps(notice,ensure_ascii=False)+','+source[match.end():]
