from datetime import datetime, timedelta, time
import logging
import sqlite3
import time as tm
import warnings
import pytz

from telegram import Update, ReplyKeyboardMarkup, InlineKeyboardButton, InlineKeyboardMarkup
from telegram.ext import (CommandHandler, Filters, MessageHandler, Filters,
                          Updater, CallbackContext, CallbackQueryHandler)

import settings

cold_last_value = 0
hot_last_value = 0
was_sent = False
keyboard_list = [
    ['/help', '/consumption', '/stat', '/subscribe']
]
keyboard_stat_list = [
    ['Month', 'Week', 'Graph']
]

# Logging settings
logging.basicConfig(handlers=[logging.FileHandler('bot.log', 'w', 'utf-8')],
                    format='%(asctime)s - %(levelname)s - %(message)s',
                    level=logging.INFO
                    )

def main():
    '''
    Main function.
    '''
    mybot = Updater(settings.API_KEY)
    logging.info('Bot is starting...')
    dp = mybot.dispatcher
    dp.add_handler(CommandHandler("help", talk_greet_user))
    # dp.add_handler(CommandHandler("sql", read_water_cons))
    dp.add_handler(CommandHandler("stat", get_stats))
    dp.add_handler(CommandHandler("subscribe", subscribe))
    dp.add_handler(CommandHandler('consumption', read_water_cons))
    # dp.add_handler(RegexHandler('^(Stat)$', get_stats))
    # dp.add_handler(RegexHandler('^(Month)$', get_month_stats))
    # dp.add_handler(RegexHandler('^(Week)$', get_week_stats))
    dp.add_handler(MessageHandler(Filters.regex(r'^(Month)$'), get_month_stats))
    dp.add_handler(MessageHandler(Filters.regex(r'^(Week)$'), get_week_stats))
    dp.add_handler(MessageHandler(Filters.text & ~Filters.command, talk_to_me))
    dp.add_handler(CallbackQueryHandler(set_was_sent, pattern="^(sent|)"))

    mybot.job_queue.run_repeating(check_health, interval=settings.HEALTH_CHECK_TIME_SEC)
    mybot.job_queue.run_daily(get_cons_update, time=time(hour=settings.SEND_UPDATE_HOUR, tzinfo=pytz.timezone('Europe/Moscow')))

    mybot.start_polling()
    mybot.idle()


def check_health(context: CallbackContext):
    '''
    Checks that values in database are actual.
    '''
    logging.info('Launching health check function.')
    cur_date = datetime.now()
    cold_date, cold_value, hot_date, hot_value = get_water(datetime.now())
    sec_from_last_cold_date = cur_date - cold_date
    sec_from_last_hot_date = cur_date - hot_date
    answer = ''
    if sec_from_last_cold_date.total_seconds() > settings.HEALTH_CHECK_DELAY_THRESHOLD:
        answer = 'WARNING!\nХОЛОДНАЯ вода обновлено более 2 часов назад\n({0}, значение: {1})'\
            .format(cold_date.strftime('%d.%m.%Y %H:%M:%S'), cold_value)
    if sec_from_last_hot_date.total_seconds() > settings.HEALTH_CHECK_DELAY_THRESHOLD:
        answer += '\nГОРЯЧАЯ вода обновлено более 2 часов назад\n({0}, значение: {1})'\
            .format(hot_date.strftime('%d.%m.%Y %H:%M:%S'), hot_value)
    if answer:
        logging.info(answer)
        subscribers_list = []
        
        connection = sqlite3.connect(settings.DB_PATH)
        cursor = connection.cursor()
        cursor.execute("SELECT ID FROM SUBSCRIBERS ORDER BY 1 ASC")
        rows = cursor.fetchall()
        for row in rows:
            subscribers_list.append(row[0])
        for client_id in subscribers_list:
            context.bot.sendMessage(chat_id=client_id, text=answer)
        
        cursor.close()
        connection.close()

    else:
        logging.info('Health check - OK!')


def get_cons_update(context: CallbackContext):
    '''
    Periodic function to get latest info about water consumption.
    '''
    logging.info('Launching getting consumption from database.')
    cur_date = datetime.now()
    global cold_last_value
    global hot_last_value
    global was_sent
    if cur_date.day in settings.SEND_UPDATE_DAYS_LIST and was_sent == False:
        cold_date, cold_value, hot_date, hot_value = get_water(datetime.now())
        if cold_value != cold_last_value or hot_value != hot_last_value:
            answer = 'ХОЛОДНАЯ вода:\nОбновлено: {0}, значение: {1}'\
                .format(cold_date.strftime('%d.%m.%Y %H:%M:%S'), cold_value)
            answer += '\nГОРЯЧАЯ вода:\nОбновлено: {0}, значение: {1}'\
                .format(hot_date.strftime('%d.%m.%Y %H:%M:%S'), hot_value)
            logging.info(answer)
            subscribers_list = []
            
            connection = sqlite3.connect(settings.DB_PATH)
            cursor = connection.cursor()
            cursor.execute("SELECT ID FROM SUBSCRIBERS ORDER BY 1 ASC")
            rows = cursor.fetchall()
            for row in rows:
                subscribers_list.append(row[0])
            for client_id in subscribers_list:
                context.bot.sendMessage(chat_id=client_id, text=answer)
                tm.sleep(0.5)
                context.bot.sendMessage(chat_id=client_id, text='Отправьте данные на сайте:\nhttps://mos.ru/services/pokazaniya-vodi-i-tepla/new/\
                                            \n\nДанные отправлены?', reply_markup=cons_send_inline_keyboard())
            
            cold_last_value = cold_value
            hot_last_value = hot_value

            cursor.close()
            connection.close()

        else:
            logging.info('Nothing changed.')
    elif cur_date.day not in settings.SEND_UPDATE_DAYS_LIST:
        was_sent = False      
        logging.info('Consumption data will be send on dates: {}.'.format(settings.SEND_UPDATE_DAYS_LIST))
    

def talk_greet_user(update: Update, context: CallbackContext):
    '''
    /help handler function.
    Greets user and provide to him breaf info about bot usage. 
    '''
    text = 'Command /help launched.'
    logging.info(text)
    # update.message.reply_text('Бот, кажется, работает...')
    my_keyboard = ReplyKeyboardMarkup(keyboard_list, resize_keyboard=True)
    update.message.reply_text('/consumption - последние данные о потреблении;\n/stat - статистика потребления за период; \
        \n/subscribe - подписаться на автоматические уведомления о потреблении воды.', reply_markup=my_keyboard)

def get_stats(update: Update, context: CallbackContext):
    '''
    /stat handler function.
    Asks user to select period for water consumption.
    '''
    text = 'Command /stat launched.'
    logging.info(text)
    my_keyboard_stat = ReplyKeyboardMarkup(keyboard_stat_list, resize_keyboard=True)
    update.message.reply_text('Выберите период', reply_markup=my_keyboard_stat)


def get_month_stats(update: Update, context: CallbackContext):
    '''
    Sends to user month stat of water consumption. 
    '''
    text = 'Month statistics launched.'
    logging.info(text)
    cold_date, cold_value, hot_date, hot_value = get_water(datetime.now(), 30)
    # print('Month cons: ', cold_value, hot_value)
    answer = 'Сумма потребления за месяц:\nхолодная: {0},\nгорячая: {1}.'.format(cold_value, hot_value)
    logging.info(answer)
    my_keyboard = ReplyKeyboardMarkup(keyboard_list, resize_keyboard=True)
    update.message.reply_text(answer, reply_markup=my_keyboard)


def get_week_stats(update: Update, context: CallbackContext):
    '''
    Sends to user week stat of water consumption. 
    '''
    text = 'Week statistics launched.'
    logging.info(text)
    cold_date, cold_value, hot_date, hot_value = get_water(datetime.now(), 7)
    # print('Week cons: ', cold_value, hot_value)
    answer = 'Сумма потребления за неделю:\nхолодная: {0},\nгорячая: {1}.'.format(cold_value, hot_value)
    logging.info(answer)
    my_keyboard = ReplyKeyboardMarkup(keyboard_list, resize_keyboard=True)
    update.message.reply_text(answer, reply_markup=my_keyboard)


def select_max_value(target_date):
    '''
    Get latest amount of water usage and date of last measures.
    '''
    connection = sqlite3.connect(settings.DB_PATH)
    cursor = connection.cursor()
    # data_items=[]
    logging.debug('SQL called. Target date: {}'.format(target_date))
    cursor.execute('WITH \
        max_cold AS (SELECT MAX(DATE) AS DATE_COLD, VALUE AS VALUE_COLD \
            FROM "WATER_VW" WHERE WATER_TYPE = 1 AND DATE <= "{0}"), \
                max_hot AS (SELECT MAX(DATE) AS DATE_HOT, VALUE AS VALUE_HOT \
                    FROM "WATER_VW" WHERE WATER_TYPE = 2 AND DATE <= "{0}") \
                        SELECT * FROM max_cold, max_hot  \
                                        '.format(target_date))
    rows = cursor.fetchall()
    cursor.close()
    connection.close()
    # print(rows)
    cold_date, cold_value, hot_date, hot_value = rows[-1]
    return cold_date, cold_value, hot_date, hot_value


def get_water(date, delta=0):
    '''
    Calculates consumption for a given period: [date-delta; date].
    Returns consumption for both cold and hot water for analiyzed period and dates for last measures.
    '''
    end_period_date = datetime.strftime(date, '%Y-%m-%dT%H:%M:%S') 
    cold_date_1, cold_value_1, hot_date_1, hot_value_1 = select_max_value(end_period_date)
    logging.info(str(cold_date_1) + '; ' + str(cold_value_1) + '; ' + str(hot_date_1) + '; ' + str(hot_value_1))
    # print(cold1, hot1, date1)
    cold_value_2, hot_value_2 = 0, 0

    if delta > 0:
        start_period_date = datetime.strftime(date-timedelta(days=delta), '%Y-%m-%dT%H:%M:%S')
        cold_date_2, cold_value_2, hot_date_2, hot_value_2 = select_max_value(start_period_date)
        logging.info(str(cold_date_2) + '; ' + str(cold_value_2) + '; ' + str(hot_date_2) + '; ' + str(hot_value_2))

    return datetime.strptime(cold_date_1, '%Y-%m-%dT%H:%M:%S.%fZ'), cold_value_1 - cold_value_2, datetime.strptime(hot_date_1, '%Y-%m-%dT%H:%M:%S.%fZ'), hot_value_1 - hot_value_2


def talk_to_me(update: Update, context: CallbackContext):
    '''
    Returns to user his own's message.
    '''
    user_text = update.message.text 
    # print(user_text)
    logging.info(user_text)
    update.message.reply_text(user_text)


def read_water_cons(update: Update, context: CallbackContext):
    '''
    /consumption handler function.
    Sends to user latest data abust consumption and date of last measures.
    '''
    text = 'Command /consumption launched.'
    logging.info(text)
    cold_date, cold_value, hot_date, hot_value = get_water(datetime.now())
    answer = 'ХОЛОДНАЯ вода:\nОбновлено: {0}, значение: {1}'\
        .format(cold_date.strftime('%d.%m.%Y %H:%M:%S'), cold_value)
    answer += '\nГОРЯЧАЯ вода:\nОбновлено: {0}, значение: {1}'\
        .format(hot_date.strftime('%d.%m.%Y %H:%M:%S'), hot_value)
    logging.info(answer)
    subscribers_list = []
    connection = sqlite3.connect(settings.DB_PATH)
    cursor = connection.cursor()
    cursor.execute("SELECT ID FROM SUBSCRIBERS ORDER BY 1 ASC")
    rows = cursor.fetchall()
    for row in rows:
        subscribers_list.append(row[0])
    for client_id in subscribers_list:
        context.bot.sendMessage(chat_id=client_id, text=answer)
    
    cursor.close()
    connection.close()


def subscribe(update: Update, context: CallbackContext):
    '''
    /subscribe handler function.
    Adds user to subscribers list.
    '''
    text = 'Command /subscribe launched.'
    logging.info(text)
    subscribers_list = []
    connection = sqlite3.connect(settings.DB_PATH)
    cursor = connection.cursor()
    cursor.execute("SELECT ID FROM SUBSCRIBERS ORDER BY 1 ASC")
    rows = cursor.fetchall()
    for row in rows:
        subscribers_list.append(row[0])

    if update.message.chat_id not in subscribers_list:
        cursor.execute('INSERT INTO SUBSCRIBERS VALUES({}, "{}")'.format(update.message.chat_id, str(datetime.now())))
        connection.commit()
        logging.info('Добавлен новый chat_id: {}, от {}'.format(update.message.chat_id, str(datetime.now())))
        update.message.reply_text('Вы добавлены в список рассылки уведомлений о новых данных потребления воды.')
    else:
        update.message.reply_text('Вы уже в списке рассылки о новых данных потребления воды.')

    cursor.close()    
    connection.close()


def cons_send_inline_keyboard():
    inlinekeyboard = [
        [
            InlineKeyboardButton('Да', callback_data='sent|1'),
            InlineKeyboardButton('Нет', callback_data='sent|0')
        ]
    ]
    return InlineKeyboardMarkup(inlinekeyboard)


def set_was_sent(update: Update, context: CallbackContext):
    global was_sent
    update.callback_query.answer()
    callback_type, was_sent_str = update.callback_query.data.split("|")
    was_sent = bool(int(was_sent_str))
    if was_sent:
        text = 'Данные отправлены на mos.ru'
        logging.info('Variable was_sent is set to True.')
    else:
        text = 'Данные НЕ отправлены на mos.ru'
        logging.info('Variable was_sent is set to False.')
    # update.callback_query.edit_message_caption(caption=text)
    update.callback_query.edit_message_text(text=text, reply_markup=None)


if __name__ == '__main__':
    main()
